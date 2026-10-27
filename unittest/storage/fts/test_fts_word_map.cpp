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
#include "share/text_analysis/ob_text_analyzer.h"
#include "storage/fts/ob_beng_ft_parser.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/retrieval/ob_sparse_daat_iter.h"
#include "sql/das/ob_das_domain_utils.h"

namespace oceanbase
{
namespace storage
{
using namespace common;

// Keep the former synchronized implementation as a semantic reference. Bucket
// counts, iteration order, collations and replacement spelling must agree.
using LockedWordMap = hash::ObHashMap<ObFTWord, int64_t>;

// The generic analyzer is the BENG pipeline before its ownership optimization.
// Keep this independent reference on its ordinary, scratch-buffer interface.
void compare_beng_tokens(const std::string &text, ObCollationType collation)
{
  SCOPED_TRACE(testing::Message() << "input size=" << text.size() << " collation=" << collation);
  ObArenaAllocator output_allocator(ObMemAttr("BengOutputTest"));
  ObArenaAllocator reference_allocator(ObMemAttr("BengRefTest"));
  ObFTParserParam param;
  param.allocator_ = &output_allocator;
  param.cs_ = ObCharset::get_charset(collation);
  param.fulltext_ = text.data();
  param.ft_length_ = text.size();
  param.parser_version_ = ObFTParser::BUILTIN_VERSION;
  share::ObTextAnalysisCtx analysis;
  analysis.cs_ = param.cs_;
  analysis.filter_stopword_ = false;
  analysis.need_grouping_ = false;
  share::ObEnglishTextAnalyzer reference;
  ObDatum document;
  document.set_string(text.data(), text.size());
  share::ObITokenStream *stream = nullptr;
  ASSERT_EQ(OB_SUCCESS, reference.init(analysis, reference_allocator));
  ASSERT_EQ(OB_SUCCESS, reference.analyze(document, stream));
  ObBEngFTParser parser(output_allocator);
  ASSERT_EQ(OB_SUCCESS, parser.init(&param));
  std::vector<std::pair<ObString, std::string>> retained;
  bool ended = false;
  for (size_t step = 0; step <= text.size(); ++step) {
    const char *word = nullptr;
    int64_t length = 0;
    int64_t characters = 0;
    int64_t frequency = 0;
    ObDatum expected;
    int64_t expected_frequency = 0;
    int expected_ret = stream->get_next(expected, expected_frequency);
    // The legacy BENG wrapper rejects an empty result from case conversion
    // even when the generic stream reports success on malformed UTF-8.
    if (OB_SUCCESS == expected_ret
        && (nullptr == expected.ptr_ || 0 == expected.len_ || expected_frequency <= 0)) {
      expected_ret = OB_INVALID_ARGUMENT;
    }
    int actual_ret = parser.get_next_token(word, length, characters, frequency);
    ASSERT_EQ(expected_ret, actual_ret);
    if (OB_SUCCESS != expected_ret) {
      EXPECT_EQ(nullptr, word);
      EXPECT_EQ(0, length);
      EXPECT_EQ(0, characters);
      EXPECT_EQ(0, frequency);
      ended = true;
      break;
    }
    ASSERT_NE(nullptr, word);
    EXPECT_EQ(expected.get_string(), ObString(length, word));
    EXPECT_EQ(expected.len_, characters);
    EXPECT_EQ(expected_frequency, frequency);
    retained.emplace_back(ObString(length, word), std::string(expected.ptr_, expected.len_));
  }
  ASSERT_TRUE(ended);
  // Hash-map keys outlive the iterator and must not refer to its scratch arena.
  parser.reset();
  reference.reset();
  reference_allocator.reset();
  for (const auto &token : retained) {
    EXPECT_EQ(ObString(token.second.size(), token.second.data()), token.first);
  }
  // Reinitialization must not leave a tokenizer open on the old document.
  const char *second = "NEXT next";
  param.fulltext_ = second;
  param.ft_length_ = STRLEN(second);
  ASSERT_EQ(OB_SUCCESS, parser.init(&param));
  const char *word = nullptr;
  int64_t length = 0;
  int64_t characters = 0;
  int64_t frequency = 0;
  ASSERT_EQ(OB_SUCCESS, parser.get_next_token(word, length, characters, frequency));
  for (const auto &token : retained) {
    EXPECT_EQ(ObString(token.second.size(), token.second.data()), token.first);
  }
}

TEST(FtsBeng, generic_pipeline_equivalence_and_owned_results)
{
  std::vector<std::string> documents = {
      "The Quick BROWN fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization.",
      "Résumé CAFÉ İ I ß Σ σ ς Кириллица ＡＢＣ 数据库 𠀀 😀 END",
      "___+++... @example.com co-operate O'Reilly a_b a+b 3.14 １２３",
      "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz",
      std::string("first\0SECOND\nthird\tfourth\r\n", 27),
      " \t\r\n\v\f!@#$%^&*()_-+={}[]:;\"'<>,.?/\\|~`",
      std::string(65537, 'A') + " ending"};
  for (int byte = 0; byte < 256; ++byte) {
    documents.push_back("begin" + std::string(1, static_cast<char>(byte)) + "END");
  }
  for (ObCollationType collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
                                     CS_TYPE_UTF8MB4_UNICODE_CI, CS_TYPE_BINARY}) {
    for (const auto &document : documents) {
      compare_beng_tokens(document, collation);
    }
  }
}

TEST(FtsBeng, generated_byte_sequences_keep_existing_error_semantics)
{
  uint32_t state = 0x4f534654;
  for (int document = 0; document < 160; ++document) {
    std::string text;
    for (int byte = 0; byte < 257; ++byte) {
      state = state * 1664525U + 1013904223U;
      text.push_back(static_cast<char>(state >> 24));
    }
    compare_beng_tokens(text, CS_TYPE_UTF8MB4_BIN);
    compare_beng_tokens(text, CS_TYPE_BINARY);
  }
}

TEST(FtsBeng, parser_lifecycle_errors)
{
  ObArenaAllocator allocator(ObMemAttr("BengOutputTest"));
  ObBEngFTParser parser(allocator);
  const char *word = nullptr;
  int64_t length = 0;
  int64_t characters = 0;
  int64_t frequency = 0;
  EXPECT_EQ(OB_NOT_INIT, parser.get_next_token(word, length, characters, frequency));
  EXPECT_EQ(OB_INVALID_ARGUMENT, parser.init(nullptr));
  parser.reset();
  parser.reset();
  ObFTParserParam param;
  param.allocator_ = &allocator;
  param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  param.fulltext_ = "first second";
  param.ft_length_ = STRLEN(param.fulltext_);
  param.parser_version_ = ObFTParser::BUILTIN_VERSION;
  ASSERT_EQ(OB_SUCCESS, parser.init(&param));
  EXPECT_EQ(OB_INIT_TWICE, parser.init(&param));
  ASSERT_EQ(OB_SUCCESS, parser.get_next_token(word, length, characters, frequency));
  EXPECT_EQ(ObString("first"), ObString(length, word));
  parser.reset();
  EXPECT_EQ(OB_NOT_INIT, parser.get_next_token(word, length, characters, frequency));
}

TEST(FtsStopWords, dictionary_equivalence_for_ascii_unicode_and_padding)
{
  ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  hash::ObHashSet<ObFTWord> reference;
  ASSERT_EQ(OB_SUCCESS, reference.create(37, "FtsStopRef", "FtsStopRef"));
  ObObjMeta dictionary_meta;
  dictionary_meta.set_varchar();
  dictionary_meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  for (const auto &word : ob_stop_word_list) {
    ASSERT_EQ(OB_SUCCESS, reference.set_refactored(ObFTWord(STRLEN(word), word, dictionary_meta)));
  }
  std::vector<std::string> inputs = {"thé", "THÉ", "ｔｈｅ", "the\t", "the\n", "the  ", "thex",
      "数据库", "😀", "tһе", "tһе ", std::string("the\0", 4), std::string("\0the", 4),
      "the\xCC\x81", "THE\xC2\xA0", "the\xE3\x80\x80", "", " ", "   "};
  for (const auto &word : ob_stop_word_list) {
    std::string base(word);
    for (unsigned mask = 0; mask < (1U << base.size()); ++mask) {
      std::string mixed = base;
      for (size_t i = 0; i < mixed.size(); ++i) {
        if (mask & (1U << i)) { mixed[i] -= 'a' - 'A'; }
      }
      inputs.push_back(mixed);
      inputs.push_back(mixed + "  ");
    }
  }
  for (char a = 'a'; a <= 'z'; ++a) {
    for (char b = 'a'; b <= 'z'; ++b) {
      for (char c = 'a'; c <= 'z'; ++c) {
        inputs.push_back(std::string{a, b, c});
      }
    }
  }
  for (int value = 0; value < 256; ++value) {
    inputs.push_back("th" + std::string(1, static_cast<char>(value)));
    inputs.push_back(std::string(1, static_cast<char>(value)) + "the");
  }
  for (ObCollationType collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
                                     CS_TYPE_UTF8MB4_UNICODE_CI, CS_TYPE_BINARY}) {
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(collation);
    for (const auto &input : inputs) {
      ObArenaAllocator conversion(ObMemAttr("FtsStopRef"));
      ObString converted;
      bool actual = false;
      int expected_ret = input.empty() ? OB_INVALID_ARGUMENT : ObCharset::charset_convert(
          conversion, ObString(input.size(), input.data()), collation,
          CS_TYPE_UTF8MB4_GENERAL_CI, converted);
      bool expected = false;
      if (OB_SUCCESS == expected_ret) {
        int found = reference.exist_refactored(ObFTWord(converted.length(), converted.ptr(), dictionary_meta));
        ASSERT_TRUE(found == OB_HASH_EXIST || found == OB_HASH_NOT_EXIST);
        expected = found == OB_HASH_EXIST;
      }
      ASSERT_EQ(expected_ret, checker.check_stopword(ObFTWord(input.size(), input.data(), meta), actual));
      if (OB_SUCCESS == expected_ret) {
        ASSERT_EQ(expected, actual) << "input size=" << input.size() << " collation=" << collation;
      }
    }
  }
}

TEST(FtsMerge, id_comparison_matches_datum_functions)
{
  const auto sign = [](int64_t value) { return (value > 0) - (value < 0); };
  for (ObObjType type : {ObUInt64Type, ObVarcharType}) {
    for (ObCollationType collation : {CS_TYPE_BINARY, CS_TYPE_UTF8MB4_BIN,
                                      CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_UNICODE_CI}) {
      ObArenaAllocator allocator(ObMemAttr("FtsMergeTest"));
      std::vector<std::string> strings = {"", " ", "a", "a ", "a  ", "A", "é", "e", "😀",
                                         std::string("a\0", 2), std::string("a\0b", 3)};
      uint32_t state = 123456789;
      for (int n = 0; n < 100; ++n) {
        std::string value;
        for (int i = 0; i < n % 33; ++i) {
          state = state * 1664525U + 1013904223U;
          value.push_back(static_cast<char>(state >> 24));
        }
        strings.push_back(value);
      }
      std::vector<ObDatum> datums(strings.size() + 1);
      std::vector<uint64_t> integers(strings.size());
      datums[0].set_null();
      for (size_t i = 0; i < strings.size(); ++i) {
        if (ObUInt64Type == type) {
          datums[i + 1].ptr_ = reinterpret_cast<const char *>(&integers[i]);
          datums[i + 1].set_uint(i == 0 ? UINT64_MAX : i * UINT64_C(0x100000001));
        } else {
          datums[i + 1].set_string(strings[i].data(), strings[i].size());
        }
      }
      ObFixedArray<const ObDatum *, ObIAllocator> ids;
      ids.set_allocator(&allocator);
      ASSERT_EQ(OB_SUCCESS, ids.init(datums.size()));
      ASSERT_EQ(OB_SUCCESS, ids.prepare_allocate(datums.size()));
      for (size_t i = 0; i < datums.size(); ++i) { ids[i] = &datums[i]; }
      ObSRMergeCmp comparison;
      sql::ObDatumMeta meta;
      meta.type_ = type;
      meta.cs_type_ = collation;
      ASSERT_EQ(OB_SUCCESS, comparison.init(meta, &ids));
      auto reference = ObDatumFuncs::get_basic_func(type, collation)->null_first_cmp_;
      for (size_t left = 0; left < datums.size(); ++left) {
        for (size_t right = 0; right < datums.size(); ++right) {
          ObSRMergeItem l;
          ObSRMergeItem r;
          l.iter_idx_ = left;
          r.iter_idx_ = right;
          int expected = 0;
          int64_t actual = 0;
          ASSERT_EQ(OB_SUCCESS, reference(datums[left], datums[right], expected, nullptr));
          ASSERT_EQ(OB_SUCCESS, comparison.cmp(l, r, actual));
          ASSERT_EQ(sign(expected), sign(actual));
        }
      }
    }
  }
}

TEST(FtsIndexRows, both_layouts_match_segmented_words)
{
  ASSERT_EQ(OB_SUCCESS, ObFTParseData::init_global());
  {
    std::vector<std::string> texts = {"", "the with the", "DATABASE database café CAFE 数据库",
        "Résumé resume RESUME word_a word_b word_c", "BEGIN\nnext\tEND"};
    std::string many;
    for (int i = 0; i < 320; ++i) { many += " word" + std::to_string(i); }
    texts.push_back(many);
    for (ObCollationType collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
                                       CS_TYPE_UTF8MB4_UNICODE_CI}) {
      for (bool inverted : {false, true}) {
        for (const auto &text : texts) {
          ObArenaAllocator allocator(ObMemAttr("FtsRowsTest"));
          ObFTParseHelper helper;
          ObObjMeta meta;
          meta.set_varchar();
          meta.set_collation_type(collation);
          ASSERT_EQ(OB_SUCCESS, helper.init(&allocator, ObString("beng.1"), ObString()));
          ObDatum doc_id;
          const std::string id("document\0id", 11);
          doc_id.set_string(id.data(), id.size());
          ObFTWordMap reference;
          const int64_t buckets = MIN(MAX(text.size() / 10, 2), 997);
          ASSERT_EQ(OB_SUCCESS, reference.create(buckets, ObMemAttr("FtsRowsTest")));
          int64_t document_length = 0;
          if (!text.empty()) {
            ASSERT_EQ(OB_SUCCESS, helper.segment(meta, text.data(), text.size(), document_length, reference));
          }
          sql::ObDomainIndexRow rows;
          int ret = sql::ObDASDomainUtils::generate_fulltext_word_rows(allocator, &helper, meta,
              doc_id, ObString(text.size(), text.data()), inverted, rows);
          ASSERT_EQ(reference.empty() ? OB_ITER_END : OB_SUCCESS, ret);
          ASSERT_EQ(reference.size(), rows.count());
          int index = 0;
          for (auto it = reference.begin(); it != reference.end(); ++it, ++index) {
            auto *row = rows.at(index);
            ASSERT_TRUE(row->is_valid());
            ASSERT_EQ(4, row->get_column_count());
            EXPECT_EQ(it->first.get_word().get_string(), row->storage_datums_[inverted ? 0 : 1].get_string());
            EXPECT_EQ(ObString(id.size(), id.data()), row->storage_datums_[inverted ? 1 : 0].get_string());
            EXPECT_EQ(it->second, row->storage_datums_[2].get_uint64());
            EXPECT_EQ(document_length, row->storage_datums_[3].get_uint64());
            row->~ObDatumRow();
          }
        }
      }
    }
  }
  ObFTParseData::deinit_global();
}

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
