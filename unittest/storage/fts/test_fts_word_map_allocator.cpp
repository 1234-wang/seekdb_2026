/*
 * Copyright (c) 2025 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>
#include <string>
#include <unordered_set>
#include <vector>
#include "data_plane/fts/ob_fts_struct.h"

namespace oceanbase
{
namespace storage
{
using namespace common;

class TrackedWordAllocator final : public ObIAllocator
{
public:
  TrackedWordAllocator() : heap_(ObMemAttr("FtsAllocTest")) {}
  ~TrackedWordAllocator() override
  {
    for (void *pointer : live_) { heap_.free(pointer); }
  }
  void *alloc(int64_t size) override
  {
    if (remaining_ == 0) { return nullptr; }
    if (remaining_ > 0) { --remaining_; }
    void *pointer = heap_.alloc(size);
    if (pointer != nullptr) { live_.insert(pointer); }
    return pointer;
  }
  void *alloc(int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *pointer) override
  {
    if (pointer != nullptr) {
      if (live_.erase(pointer) != 1) {
        ++invalid_frees_;
      } else {
        heap_.free(pointer);
      }
    }
  }
  ObMalloc heap_;
  std::unordered_set<void *> live_;
  int64_t remaining_ = -1;
  int invalid_frees_ = 0;
};

TEST(FtsWordAllocator, caller_ownership_reuse_and_recreate)
{
  TrackedWordAllocator first;
  TrackedWordAllocator second;
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  std::vector<std::string> words;
  for (int i = 0; i < 400; ++i) { words.push_back("term" + std::to_string(i)); }
  {
    ObFTWordMap map;
    ASSERT_EQ(OB_SUCCESS, map.create(2, first));
    ASSERT_EQ(OB_INIT_TWICE, map.create(17, second));
    for (int i = 0; i < words.size(); ++i) {
      ASSERT_EQ(OB_SUCCESS, map.set_refactored(ObFTWord(words[i].size(), words[i].data(), meta), i + 1));
    }
    EXPECT_EQ(401, first.live_.size()); // one bucket allocation, one per node
    EXPECT_TRUE(second.live_.empty());
    ASSERT_EQ(OB_SUCCESS, map.reuse());
    EXPECT_EQ(1, first.live_.size());
    EXPECT_EQ(0, map.size());
    ASSERT_EQ(OB_SUCCESS, map.destroy());
    EXPECT_TRUE(first.live_.empty());
    ASSERT_EQ(OB_SUCCESS, map.create(17, second));
    ASSERT_EQ(OB_SUCCESS, map.set_refactored(ObFTWord(4, "word", meta), 7));
    EXPECT_EQ(2, second.live_.size());
    ASSERT_EQ(OB_SUCCESS, map.destroy());
    EXPECT_TRUE(second.live_.empty());
    // The original allocator-label overload must work after a bound map dies.
    ASSERT_EQ(OB_SUCCESS, map.create(3, ObMemAttr("FtsAllocTest")));
    ASSERT_EQ(OB_SUCCESS, map.set_refactored(ObFTWord(4, "word", meta), 9));
    EXPECT_TRUE(first.live_.empty());
    EXPECT_TRUE(second.live_.empty());
  }
  EXPECT_EQ(0, first.invalid_frees_);
  EXPECT_EQ(0, second.invalid_frees_);
}

TEST(FtsWordAllocator, allocation_failures_release_every_owned_block)
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  std::vector<std::string> words;
  for (int i = 0; i < 24; ++i) { words.push_back("term" + std::to_string(i)); }
  for (int failure_point = 0; failure_point < 25; ++failure_point) {
    TrackedWordAllocator allocator;
    {
      ObFTWordMap map;
      allocator.remaining_ = failure_point;
      int ret = map.create(2, allocator);
      if (OB_SUCCESS == ret) {
        for (const auto &word : words) {
          ret = map.set_refactored(ObFTWord(word.size(), word.data(), meta), 1);
          if (ret != OB_SUCCESS) { break; }
        }
      }
      ASSERT_EQ(OB_ALLOCATE_MEMORY_FAILED, ret);
    }
    EXPECT_TRUE(allocator.live_.empty()) << failure_point;
    EXPECT_EQ(0, allocator.invalid_frees_) << failure_point;
  }
}

TEST(FtsWordAllocator, failed_create_can_retry_or_use_original_allocator)
{
  TrackedWordAllocator allocator;
  ObFTWordMap map;
  allocator.remaining_ = 0;
  ASSERT_EQ(OB_ALLOCATE_MEMORY_FAILED, map.create(2, allocator));
  EXPECT_FALSE(map.created());
  ASSERT_EQ(OB_SUCCESS, map.create(2, ObMemAttr("FtsAllocTest")));
  EXPECT_TRUE(allocator.live_.empty());
  ASSERT_EQ(OB_SUCCESS, map.destroy());
  allocator.remaining_ = -1;
  ASSERT_EQ(OB_SUCCESS, map.create(2, allocator));
  EXPECT_EQ(1, allocator.live_.size());
}

TEST(FtsWordAllocator, dispatch_cache_follows_type_and_collation_changes)
{
  FTWordMapHash hash;
  FTWordMapEqual equal;
  const std::vector<std::string> inputs = {"a", "A", "a ", "résumé", "resume", "数据库",
                                          std::string("word\0tail", 9)};
  for (int cycle = 0; cycle < 3; ++cycle) {
    for (ObObjType type : {ObVarcharType, ObCharType}) {
      for (ObCollationType collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
                                         CS_TYPE_UTF8MB4_UNICODE_CI, CS_TYPE_BINARY}) {
        ObObjMeta meta;
        meta.set_type(type);
        meta.set_collation_type(collation);
        for (const auto &left : inputs) {
          ObFTWord l(left.size(), left.data(), meta);
          uint64_t expected = 0;
          uint64_t actual = 0;
          ASSERT_EQ(OB_SUCCESS, l.hash(expected));
          ASSERT_EQ(OB_SUCCESS, hash(l, actual));
          EXPECT_EQ(expected, actual);
          for (const auto &right : inputs) {
            ObFTWord r(right.size(), right.data(), meta);
            EXPECT_EQ(l == r, equal(l, r));
          }
        }
      }
    }
  }
}

} // namespace storage
} // namespace oceanbase
