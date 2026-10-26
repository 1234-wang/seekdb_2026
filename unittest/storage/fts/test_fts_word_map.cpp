/*
 * Copyright (c) 2025 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "lib/charset/ob_charset.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_stop_word.h"

namespace oceanbase
{
namespace storage
{
using namespace common;

// Keep the former synchronized implementation as a semantic reference. Bucket
// counts, iteration order, collations and replacement spelling must agree.
using LockedWordMap = hash::ObHashMap<ObFTWord, int64_t>;

struct AddFrequency
{
  AddFrequency(const ObFTWord &word, int64_t frequency) : word_(word), frequency_(frequency) {}
  void operator()(hash::HashMapPair<ObFTWord, int64_t> &entry) const
  {
    entry.first = word_;
    entry.second += frequency_;
  }
  const ObFTWord &word_;
  int64_t frequency_;
};

TEST(FtsWordMap, collisions_replacement_and_reuse)
{
  for (ObCollationType collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(collation);
    ObFTWordMap words;
    LockedWordMap reference;
    // Force long collision chains and cross multiple node-allocation blocks.
    ASSERT_EQ(OB_SUCCESS, words.create(2, ObMemAttr("FtsMapTest")));
    ASSERT_EQ(OB_SUCCESS, reference.create(2, ObMemAttr("FtsMapTest")));
    std::vector<std::string> tokens = {"resume", "résumé", "RESUME", "café", "CAFE",
                                      "数据库", "😀", std::string("ab\0cd", 5), "trailing ", "trailing"};
    for (int i = 0; i < 320; ++i) {
      tokens.push_back("word" + std::to_string(i));
    }
    for (int document = 0; document < 3; ++document) {
      ASSERT_EQ(OB_SUCCESS, words.reuse());
      ASSERT_EQ(OB_SUCCESS, reference.reuse());
      ASSERT_EQ(0, words.size());
      for (int pass = 0; pass < 3; ++pass) {
        for (const auto &token : tokens) {
          ObFTWord word(token.size(), token.data(), meta);
          AddFrequency add(word, pass + 1);
          ASSERT_EQ(OB_SUCCESS, words.set_or_update(word, 1, add));
          ASSERT_EQ(OB_SUCCESS, reference.set_or_update(word, 1, add));
        }
      }
      ASSERT_EQ(reference.size(), words.size());
      auto actual = words.begin();
      for (auto expected = reference.begin(); expected != reference.end(); ++expected, ++actual) {
        ASSERT_TRUE(actual != words.end());
        EXPECT_EQ(expected->first.get_word().get_string(), actual->first.get_word().get_string());
        EXPECT_EQ(expected->second, actual->second);
        int64_t frequency = 0;
        ASSERT_EQ(OB_SUCCESS, words.get_refactored(expected->first, frequency));
        EXPECT_EQ(expected->second, frequency);
      }
      EXPECT_TRUE(actual == words.end());
    }
    ASSERT_EQ(OB_SUCCESS, words.destroy());
    ASSERT_EQ(OB_SUCCESS, words.create(17, ObMemAttr("FtsMapTest")));
    EXPECT_EQ(0, words.size());
  }
}

TEST(FtsWordMap, grouping_keeps_document_length_and_last_spelling)
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ObArenaAllocator allocator(ObMemAttr("FtsMapTest"));
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(2, ObMemAttr("FtsMapTest")));
  ObFTParserProperty property;
  ObAddWordFlag flag;
  flag.set_groupby_word();
  ObAddWord add(property, meta, flag, allocator, words);
  ASSERT_EQ(OB_SUCCESS, add.process_word("resume", 6, 6, 1));
  ASSERT_EQ(OB_SUCCESS, add.process_word("résumé", 8, 6, 2));
  ASSERT_EQ(OB_SUCCESS, add.process_word("RESUME", 6, 6, 3));
  EXPECT_EQ(6, add.get_add_word_count());
  ASSERT_EQ(1, words.size());
  EXPECT_EQ(6, words.begin()->second);
  EXPECT_EQ(ObString("RESUME"), words.begin()->first.get_word().get_string());
}

TEST(FtsStopWords, collation_conversion_and_lifecycle)
{
  ObStopWordChecker checker;
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  bool found = false;
  ObFTWord the(3, "THE", meta);
  EXPECT_EQ(OB_NOT_INIT, checker.check_stopword(the, found));
  for (int cycle = 0; cycle < 3; ++cycle) {
    ASSERT_EQ(OB_SUCCESS, checker.init());
    EXPECT_EQ(OB_INIT_TWICE, checker.init());
    for (const auto &token : {"the", "THE", "thé", "with", "WITH", "where", "WHERE"}) {
      ASSERT_EQ(OB_SUCCESS, checker.check_stopword(ObFTWord(STRLEN(token), token, meta), found));
      EXPECT_TRUE(found) << token;
    }
    for (const auto &token : {"database", "数据库", "😀", "without"}) {
      ASSERT_EQ(OB_SUCCESS, checker.check_stopword(ObFTWord(STRLEN(token), token, meta), found));
      EXPECT_FALSE(found) << token;
    }
    EXPECT_EQ(OB_INVALID_ARGUMENT, checker.check_stopword(ObFTWord(), found));
    meta.set_collation_type(CS_TYPE_BINARY);
    ASSERT_EQ(OB_SUCCESS, checker.check_stopword(ObFTWord(4, "thé", meta), found));
    EXPECT_TRUE(found);
    // This competition branch only supports binary and UTF-8 collations.
    meta.set_collation_type(CS_TYPE_LATIN1_SWEDISH_CI);
    EXPECT_EQ(OB_INVALID_ARGUMENT, checker.check_stopword(ObFTWord(3, "the", meta), found));
    meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    checker.destroy();
    checker.destroy();
    EXPECT_EQ(OB_NOT_INIT, checker.check_stopword(the, found));
  }
}

TEST(FtsStopWords, concurrent_reads_of_published_dictionary)
{
  ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  std::atomic<int> failures{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 8; ++i) {
    readers.emplace_back([&]() {
      ObObjMeta meta;
      meta.set_varchar();
      meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      for (int repeat = 0; repeat < 1000; ++repeat) {
        for (const auto &token : {"the", "THE", "thé", "database", "数据库"}) {
          bool found = false;
          int ret = checker.check_stopword(ObFTWord(STRLEN(token), token, meta), found);
          bool expected = token[0] == 't' || token[0] == 'T';
          if (OB_SUCCESS != ret || found != expected) {
            ++failures;
          }
        }
      }
    });
  }
  for (auto &reader : readers) {
    reader.join();
  }
  EXPECT_EQ(0, failures.load());
}

TEST(FtsWordMap, beng_segment_reuses_map_across_documents)
{
  ASSERT_EQ(OB_SUCCESS, ObFTParseData::init_global());
  {
    ObArenaAllocator allocator(ObMemAttr("FtsMapTest"));
    ObFTParseHelper helper;
    ObFTWordMap words;
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, helper.init(&allocator, ObString("beng.1"), ObString()));
    ASSERT_EQ(OB_SUCCESS, words.create(2, ObMemAttr("FtsMapTest")));
    for (int repeat = 0; repeat < 100; ++repeat) {
      const char *text = "The DATABASE database with café CAFE database";
      int64_t document_length = 0;
      ASSERT_EQ(OB_SUCCESS, helper.segment(meta, text, STRLEN(text), document_length, words));
      EXPECT_EQ(5, document_length);
      EXPECT_EQ(3, words.size());
      int64_t frequency = 0;
      ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(8, "database", meta), frequency));
      EXPECT_EQ(3, frequency);
      text = "the with the";
      ASSERT_EQ(OB_SUCCESS, helper.segment(meta, text, STRLEN(text), document_length, words));
      EXPECT_EQ(0, document_length);
      EXPECT_EQ(0, words.size());
      allocator.reuse();
    }
  }
  ObFTParseData::deinit_global();
}

} // namespace storage
} // namespace oceanbase

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  if (oceanbase::common::OB_SUCCESS != oceanbase::common::ObCharset::init_charset()) {
    return 1;
  }
  return RUN_ALL_TESTS();
}
