/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_FTS_STRUCT_H_
#define OB_FTS_STRUCT_H_

#include "lib/charset/ob_charset.h"
#include "lib/hash/ob_hashmap.h"
#include "object/ob_object.h"
#include "share/datum/ob_datum_funcs.h"

namespace oceanbase
{
namespace storage
{

class ObFTWord final
{
public:
  ObFTWord() : word_(), meta_() {}
  ObFTWord(const int64_t length, const char *ptr, const ObObjMeta &meta) : meta_(meta)
  {
    word_.set_string(ptr, length);
  }
  ~ObFTWord() = default;

  OB_INLINE const ObDatum &get_word() const { return word_; }
  OB_INLINE const ObObjMeta &get_meta() const { return meta_; }
  OB_INLINE ObCollationType get_collation_type() const { return meta_.get_collation_type(); }
  OB_INLINE bool empty() const { return word_.get_string().empty(); }
  int hash(uint64_t &hash_val) const;
  bool operator==(const ObFTWord &other) const;
  OB_INLINE bool operator !=(const ObFTWord &other) const { return !(other == *this); }

  TO_STRING_KV(K_(meta), K_(word));

private:
  ObDatum word_;
  ObObjMeta meta_;
};

// A word map is confined to one worker. Cache operation dispatch there rather
// than adding cached fields to every token or changing collation semantics.
class FTWordMapHash final
{
public:
  int operator()(const ObFTWord &word, uint64_t &hash) const
  {
    if (function_ == nullptr || meta_ != word.get_meta()) {
      meta_ = word.get_meta();
      auto *functions = common::ObDatumFuncs::get_basic_func(meta_.get_type(), meta_.get_collation_type());
      function_ = functions != nullptr ? functions->default_hash_ : nullptr;
    }
    return function_ != nullptr ? function_(word.get_word(), 0, hash, nullptr) : OB_ERR_UNEXPECTED;
  }
private:
  mutable common::ObObjMeta meta_;
  mutable common::ObDatumHashFuncType function_ = nullptr;
};

class FTWordMapEqual final
{
public:
  bool operator()(const ObFTWord &left, const ObFTWord &right) const
  {
    if (function_ == nullptr || left_meta_ != left.get_meta() || right_meta_ != right.get_meta()) {
      resolve(left.get_meta(), right.get_meta());
    }
    int compared = 0;
    if (OB_SUCCESS != function_(left.get_word(), right.get_word(), compared, nullptr)) {
      ob_abort();
    }
    return compared == 0;
  }
private:
  void resolve(const ObObjMeta &left, const ObObjMeta &right) const;
  mutable common::ObObjMeta left_meta_;
  mutable common::ObObjMeta right_meta_;
  mutable common::ObDatumCmpFuncType function_ = nullptr;
};

// Each map is local to document/query tokenization. Neither its buckets nor
// its node allocator are shared between workers, so per-token bucket latches
// and allocator spinlocks are unnecessary.
using FTWordMapNode = common::hash::HashMapTypes<ObFTWord, int64_t>::AllocType;
using FTWordMapNodeAllocatorBase = common::hash::SimpleAllocer<
    FTWordMapNode,
    common::hash::NodeNumTraits<FTWordMapNode>::NODE_NUM,
    common::hash::NoPthreadDefendMode>;
class FTWordMapNodeAllocator final : public FTWordMapNodeAllocatorBase
{
public:
  void set_allocator(common::ObIAllocator *allocator) { allocator_ = allocator; }
  FTWordMapNode *alloc()
  {
    FTWordMapNode *node = nullptr;
    if (allocator_ == nullptr) {
      node = FTWordMapNodeAllocatorBase::alloc();
    } else if (void *buffer = allocator_->alloc(sizeof(FTWordMapNode)); buffer != nullptr) {
      node = new (buffer) FTWordMapNode();
    }
    return node;
  }
  void free(FTWordMapNode *node)
  {
    if (allocator_ == nullptr) {
      FTWordMapNodeAllocatorBase::free(node);
    } else if (node != nullptr) {
      node->~FTWordMapNode();
      allocator_->free(node);
    }
  }
private:
  common::ObIAllocator *allocator_ = nullptr;
};

class FTWordMapBucketAllocator final : public common::ObMalloc
{
public:
  using common::ObMalloc::ObMalloc;
  void set_allocator(common::ObIAllocator *allocator) { allocator_ = allocator; }
  void *alloc(int64_t size) override
  {
    return allocator_ != nullptr ? allocator_->alloc(size) : common::ObMalloc::alloc(size);
  }
  void *alloc(int64_t size, const common::ObMemAttr &attr) override
  {
    return allocator_ != nullptr ? allocator_->alloc(size, attr) : common::ObMalloc::alloc(size, attr);
  }
  void free(void *pointer) override
  {
    if (allocator_ != nullptr) {
      allocator_->free(pointer);
    } else {
      common::ObMalloc::free(pointer);
    }
  }
private:
  common::ObIAllocator *allocator_ = nullptr;
};

typedef common::hash::ObHashMap<ObFTWord,
                              int64_t,
                              common::hash::NoPthreadDefendMode,
                              FTWordMapHash,
                              FTWordMapEqual,
                              FTWordMapNodeAllocator,
                              common::hash::NormalPointer,
                              FTWordMapBucketAllocator> FTWordMapBase;

class ObFTWordMap final : public FTWordMapBase
{
public:
  using FTWordMapBase::create;
  int create(int64_t bucket_count, common::ObIAllocator &allocator)
  {
    int ret = OB_SUCCESS;
    if (created()) {
      ret = OB_INIT_TWICE;
    } else {
      // The caller outlives this local map. Reuse its allocation pages for
      // temporary buckets and nodes rather than allocating separate blocks.
      get_local_allocer().set_allocator(&allocator);
      get_local_bucket_allocer().set_allocator(&allocator);
      ret = FTWordMapBase::create(bucket_count, common::ObMemAttr("FTWordMap"));
      if (OB_FAIL(ret)) {
        reset_parent_allocator();
      }
    }
    return ret;
  }
  int destroy()
  {
    int ret = FTWordMapBase::destroy();
    reset_parent_allocator();
    return ret;
  }
private:
  void reset_parent_allocator()
  {
    get_local_allocer().set_allocator(nullptr);
    get_local_bucket_allocer().set_allocator(nullptr);
  }
};

class ObAddWordFlag final
{
private:
  static const uint64_t AWF_NONE         = 0;
  static const uint64_t AWF_MIN_MAX_WORD = 1 << 0; // filter words that are less than a minimum or greater
                                                   // than a maximum word length.
  static const uint64_t AWF_STOPWORD     = 1 << 1; // filter by sotp word table.
  static const uint64_t AWF_CASEDOWN     = 1 << 2; // convert characters from uppercase to lowercase.
  static const uint64_t AWF_GROUPBY_WORD = 1 << 3; // distinct and word aggregation
public:
  ObAddWordFlag() : flag_(AWF_NONE) {}
  ~ObAddWordFlag() = default;
public:
  void set_flag(const uint64_t flag) { flag_ |= flag; }
private:
  void clear_flag(const uint64_t flag) { flag_ &= ~flag; }
  bool has_flag(const uint64 flag) const { return (flag_ & flag) == flag; }
public:
  void set_min_max_word() { set_flag(AWF_MIN_MAX_WORD); }
  void set_stop_word() { set_flag(AWF_STOPWORD); }
  void set_casedown() { set_flag(AWF_CASEDOWN); }
  void set_groupby_word() { set_flag(AWF_GROUPBY_WORD); }
  void clear() { flag_ = AWF_NONE; }
  void clear_min_max_word() { clear_flag(AWF_MIN_MAX_WORD); }
  void clear_stop_word() { clear_flag(AWF_STOPWORD); }
  void clear_casedown() { clear_flag(AWF_CASEDOWN); }
  void clear_groupby_word() { clear_flag(AWF_GROUPBY_WORD); }
  bool min_max_word() const { return has_flag(AWF_MIN_MAX_WORD); }
  bool stopword() const { return has_flag(AWF_STOPWORD); }
  bool casedown() const { return has_flag(AWF_CASEDOWN); }
  bool groupby_word() const { return has_flag(AWF_GROUPBY_WORD); }
  TO_STRING_KV(K_(flag));
private:
  uint64_t flag_;
};

} // end namespace storage
} // end namespace oceanbase

#endif// OB_FTS_STRUCT_H_
