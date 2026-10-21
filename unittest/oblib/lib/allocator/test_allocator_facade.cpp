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

#include <cstdint>
#include <type_traits>
#include <gtest/gtest.h>
#include "lib/allocator/ob_malloc.h"
#include "lib/allocator/page_arena.h"
#include "lib/objectpool/ob_resource_pool.h"
#include "lib/queue/ob_fixed_queue.h"
#include "lib/rc/context.h"
#if defined(OB_HAVE_BUNDLED_JEMALLOC) && defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::lib;

namespace
{

class CountingAllocator final : public ObIAllocator
{
public:
  CountingAllocator()
    : allocator_(), live_allocations_(0), live_bytes_(0), fail_next_realloc_(false)
  {}

  void *alloc(const int64_t size) override
  {
    return alloc(size, ObMemAttr());
  }

  void *alloc(const int64_t size, const ObMemAttr &attr) override
  {
    void *ptr = allocator_.alloc(size, attr);
    if (nullptr != ptr) {
      ++live_allocations_;
      live_bytes_ += ob_malloc_usable_size(ptr);
    }
    return ptr;
  }

  void *realloc(const void *ptr, const int64_t size, const ObMemAttr &attr) override
  {
    if (fail_next_realloc_) {
      fail_next_realloc_ = false;
      return nullptr;
    }
    const int64_t old_size = nullptr != ptr
        ? ob_malloc_usable_size(const_cast<void *>(ptr))
        : 0;
    void *new_ptr = allocator_.realloc(ptr, size, attr);
    if (nullptr == ptr && nullptr != new_ptr) {
      ++live_allocations_;
    }
    if (nullptr != new_ptr) {
      live_bytes_ += ob_malloc_usable_size(new_ptr) - old_size;
    }
    return new_ptr;
  }

  void free(void *ptr) override
  {
    if (nullptr != ptr) {
      live_bytes_ -= ob_malloc_usable_size(ptr);
      allocator_.free(ptr);
      --live_allocations_;
    }
  }

  int64_t live_allocations() const { return live_allocations_; }
  int64_t live_bytes() const { return live_bytes_; }
  void fail_next_realloc() { fail_next_realloc_ = true; }

private:
  ObMalloc allocator_;
  int64_t live_allocations_;
  int64_t live_bytes_;
  bool fail_next_realloc_;
};

class ArenaLikeAllocator final : public ObIAllocator
{
public:
  ArenaLikeAllocator()
    : allocator_(), allocation_(nullptr), freed_(nullptr)
  {}

  ~ArenaLikeAllocator() override
  {
    allocator_.free(allocation_);
  }

  void *alloc(const int64_t size) override
  {
    return alloc(size, ObMemAttr());
  }

  void *alloc(const int64_t size, const ObMemAttr &attr) override
  {
    allocation_ = allocator_.alloc(size, attr);
    return allocation_;
  }

  void free(void *ptr) override
  {
    // Page arenas accept individual frees but reclaim the backing page only
    // during reset/destruction.  Preserve the pointer for an identity check.
    freed_ = ptr;
  }

  void *allocation() const { return allocation_; }
  void *freed() const { return freed_; }

private:
  ObMalloc allocator_;
  void *allocation_;
  void *freed_;
};

struct ResourcePoolObject
{
  int64_t value_ = 0;
  void reset() { value_ = 0; }
};

class TestResourcePool final : public ObBaseResourcePool<ResourcePoolObject, RPStrLabel<>>
{
  using Base = ObBaseResourcePool<ResourcePoolObject, RPStrLabel<>>;
public:
  explicit TestResourcePool(ObIAllocator &allocator)
      : Base(1, &allocator, ObMemAttr("PoolOverflow"))
  {}
  using Base::set_overflow_allocator;
};

// A test-only quota checks freeable release without restoring SQL's retired
// global tracker. Arena lifetime is checked with explicit usage tracking.
class ContextTestQuota final : public MemoryQuota
{
public:
  int64_t used() const { return committed(); }
};
MemoryQuota *context_test_quota = nullptr;
MemoryQuota *resolve_context_test_quota(const int64_t)
{
  return context_test_quota;
}
class ContextQuotaGuard final
{
public:
  explicit ContextQuotaGuard(ContextTestQuota &quota)
  {
    context_test_quota = &quota;
    set_memory_quota_resolver(ObCtxIds::VECTOR_CTX_ID, resolve_context_test_quota);
  }
  ~ContextQuotaGuard()
  {
    set_memory_quota_resolver(ObCtxIds::VECTOR_CTX_ID, nullptr);
    context_test_quota = nullptr;
  }
};

} // namespace

TEST(TestResourcePoolAllocator, default_explicit_and_null_overflow_allocators)
{
  CountingAllocator inner;
  ObMalloc backing;
  MemoryUsageTracker tracker;
  TrackedAllocator tracked(backing, &tracker, nullptr, ObMemAttr("PoolOverflow"), true);
  {
    TestResourcePool pool(inner);
    ResourcePoolObject *ordinary = pool.alloc();
    ASSERT_NE(nullptr, ordinary);
    EXPECT_EQ(1, inner.live_allocations());

    // Occupying the sole ordinary slot forces all subsequent allocations
    // through the overflow path, which defaults to the process allocator.
    ResourcePoolObject *default_node = pool.alloc();
    ASSERT_NE(nullptr, default_node);
    EXPECT_EQ(0, default_node->value_);
    EXPECT_EQ(1, inner.live_allocations());
    pool.free(default_node);

    // A supplied allocator still owns overflow layout and accounting, as in
    // Meta pools. Its header must be released through that same allocator.
    pool.set_overflow_allocator(&tracked);
    ResourcePoolObject *tracked_node = pool.alloc();
    ASSERT_NE(nullptr, tracked_node);
    EXPECT_GT(tracker.used(), 0);
    pool.free(tracked_node);
    EXPECT_EQ(0, tracker.used());

    // Reset only after freeing the custom allocation. nullptr restores the
    // default rather than disabling overflow or retaining the custom owner.
    pool.set_overflow_allocator(nullptr);
    ResourcePoolObject *reset_node = pool.alloc();
    ASSERT_NE(nullptr, reset_node);
    EXPECT_EQ(0, reset_node->value_);
    EXPECT_EQ(0, tracker.used());
    EXPECT_EQ(1, inner.live_allocations());
    pool.free(reset_node);
    pool.free(ordinary);
  }
  EXPECT_EQ(0, inner.live_allocations());
  EXPECT_EQ(0, tracker.used());
}

TEST(TestMallocBackend, default_queue_allocator_is_ready_at_startup)
{
  ObFixedQueue<int> queue;
  ASSERT_EQ(OB_SUCCESS, queue.init(4));
  int value = 42;
  ASSERT_EQ(OB_SUCCESS, queue.push(&value));
  int *popped = nullptr;
  ASSERT_EQ(OB_SUCCESS, queue.pop(popped));
  EXPECT_EQ(&value, popped);
}

TEST(TestMallocBackend, allocate_reallocate_free)
{
  ObMemAttr attr;
  void *ptr = ob_malloc(100, attr);
  ASSERT_NE(nullptr, ptr);
  ASSERT_GE(ob_malloc_usable_size(ptr), 100);

  ptr = ob_realloc(ptr, 200, attr);
  ASSERT_NE(nullptr, ptr);
  ASSERT_GE(ob_malloc_usable_size(ptr), 200);
  EXPECT_EQ(nullptr, ob_realloc(ptr, 0, attr));
  EXPECT_EQ(nullptr, ob_malloc(0, attr));
  EXPECT_EQ(nullptr, ob_malloc(-1, attr));
}

TEST(TestMallocBackend, documented_mem_attr_shape_matches_header)
{
  static_assert(std::is_same<decltype(ObMemAttr::label_), ObLabel>::value,
      "ObMemAttr documentation must match label_ type");
  static_assert(std::is_same<decltype(ObMemAttr::ctx_id_), uint64_t>::value,
      "ObMemAttr documentation must match ctx_id_ type");
  static_assert(std::is_same<decltype(ObMemAttr::sub_ctx_id_), int32_t>::value,
      "ObMemAttr documentation must match sub_ctx_id_ type");
  static_assert(std::is_same<decltype(ObMemAttr::prio_), ObAllocPrio>::value,
      "ObMemAttr documentation must match prio_ type");

  ObMemAttr attr("DocExample", ObCtxIds::DEFAULT_CTX_ID, OB_NORMAL_ALLOC);
  attr.sub_ctx_id_ = 0;
  EXPECT_STREQ("DocExample", attr.label_.str_);
  EXPECT_EQ(ObCtxIds::DEFAULT_CTX_ID, attr.ctx_id_);
  EXPECT_EQ(0, attr.sub_ctx_id_);
  EXPECT_EQ(OB_NORMAL_ALLOC, attr.prio_);
}

TEST(TestMallocBackend, module_page_allocations_do_not_resolve_context_quota)
{
  ObMemAttr attr("ResolverTrans", ObCtxIds::VECTOR_CTX_ID);
  ModulePageAllocator allocator(attr);
  ContextTestQuota tracker;
  tracker.reset(0);

  void *before_resolver = allocator.alloc(64);
  ASSERT_NE(nullptr, before_resolver);
  // Without accounting the returned pointer keeps the malloc allocation layout.
  EXPECT_GE(ob_malloc_usable_size(before_resolver), 64);
  allocator.free(before_resolver);
  {
    ContextQuotaGuard tracker_guard(tracker);
    void *while_resolver = allocator.alloc(128);
    ASSERT_NE(nullptr, while_resolver);
    EXPECT_GE(ob_malloc_usable_size(while_resolver), 128);
    EXPECT_EQ(0, tracker.used());
    allocator.free(while_resolver);
    ASSERT_EQ(0, tracker.used());

    ModulePageAllocator copy(allocator);
    ModulePageAllocator assigned;
    assigned = copy;
    void *explicit_attr = assigned.alloc(256, attr);
    ASSERT_NE(nullptr, explicit_attr);
    EXPECT_GE(ob_malloc_usable_size(explicit_attr), 256);
    assigned.free(explicit_attr);
    EXPECT_EQ(0, tracker.reject_count());
  }

  EXPECT_EQ(nullptr, resolve_memory_quota(ObCtxIds::VECTOR_CTX_ID));
  void *after_resolver = allocator.alloc(256);
  ASSERT_NE(nullptr, after_resolver);
  EXPECT_GE(ob_malloc_usable_size(after_resolver), 256);
  allocator.free(after_resolver);
  ASSERT_EQ(0, tracker.used());
}

TEST(TestMallocBackend, module_page_allocator_preserves_explicit_owner_layout)
{
  ContextTestQuota tracker;
  ContextQuotaGuard tracker_guard(tracker);
  ArenaLikeAllocator arena;
  ModulePageAllocator allocator(arena, "ArenaDelegate");
  allocator.set_ctx_id(ObCtxIds::VECTOR_CTX_ID);

  void *ptr = allocator.alloc(128);
  ASSERT_NE(nullptr, ptr);
  EXPECT_EQ(arena.allocation(), ptr);
  EXPECT_EQ(0, tracker.used());

  allocator.free(ptr);
  EXPECT_EQ(ptr, arena.freed());
  EXPECT_EQ(0, tracker.used());
}

TEST(TestMallocBackend, untracked_adapter_preserves_backing_layout)
{
  ArenaLikeAllocator arena;
  ObMemAttr attr("UntrackedAlloc");
  TrackedAllocator allocator(arena, nullptr, nullptr, attr);

  void *ptr = allocator.alloc(128);
  ASSERT_NE(nullptr, ptr);
  EXPECT_EQ(arena.allocation(), ptr);
  EXPECT_EQ(ptr, allocator.raw_pointer(ptr));

  allocator.free(ptr);
  EXPECT_EQ(ptr, arena.freed());
}

TEST(TestMallocBackend, explicit_arena_tracker_does_not_require_ctx_registry)
{
  MemoryUsageTracker tracker;
  {
    ObArenaAllocator arena("ExplicitTracker");
    arena.set_memory_tracker(&tracker); // TxData uses this explicit contract.
    ASSERT_NE(nullptr, arena.alloc(4097));
    EXPECT_GT(tracker.used(), 0);
    EXPECT_EQ(arena.total(), tracker.used());
    arena.reuse();
    EXPECT_EQ(arena.total(), tracker.used());
    arena.reset();
    EXPECT_EQ(0, tracker.used());
    ASSERT_NE(nullptr, arena.alloc(1024));
  }
  EXPECT_EQ(0, tracker.used());
}

TEST(TestMallocBackend, quota_realloc_preserves_original_on_rejection)
{
  CountingAllocator backing_allocator;
  MemoryUsageTracker tracker;
  MemoryQuota quota(4096);
  ObMemAttr attr("QuotaRealloc");
  TrackedAllocator allocator(backing_allocator, &tracker, &quota, attr);

  char *ptr = static_cast<char *>(allocator.alloc(64));
  ASSERT_NE(nullptr, ptr);
  MEMSET(ptr, 0x5a, 64);
  const int64_t old_committed = quota.committed();
  ASSERT_GT(old_committed, 64);
  quota.set_limit(old_committed);

  ASSERT_EQ(nullptr, allocator.realloc(ptr, 128, attr));
  ASSERT_EQ(old_committed, quota.committed());
  ASSERT_EQ(old_committed, tracker.used());
  for (int i = 0; i < 64; ++i) {
    ASSERT_EQ(static_cast<char>(0x5a), ptr[i]);
  }

  quota.set_limit(4096);
  ptr = static_cast<char *>(allocator.realloc(ptr, 128, attr));
  ASSERT_NE(nullptr, ptr);
  for (int i = 0; i < 64; ++i) {
    ASSERT_EQ(static_cast<char>(0x5a), ptr[i]);
  }
  allocator.free(ptr);
  ASSERT_EQ(0, quota.committed());
  ASSERT_EQ(0, quota.reserved());
  ASSERT_EQ(0, tracker.used());
  ASSERT_EQ(0, backing_allocator.live_allocations());
}

TEST(TestMallocBackend, quota_realloc_admits_temporary_replacement_in_full)
{
  CountingAllocator backing_allocator;
  MemoryUsageTracker tracker;
  MemoryQuota quota(4096);
  ObMemAttr attr("QuotaRealPeak");
  TrackedAllocator allocator(backing_allocator, &tracker, &quota, attr);

  char *ptr = static_cast<char *>(allocator.alloc(64));
  ASSERT_NE(nullptr, ptr);
  MEMSET(ptr, 0x3c, 64);
  const int64_t old_committed = quota.committed();
  ASSERT_GT(old_committed, 64);

  // The replacement needs more than 127 backing bytes while the original is
  // still live.  A delta-only reservation would incorrectly admit it.
  quota.set_limit(old_committed + 127);
  ASSERT_EQ(nullptr, allocator.realloc(ptr, 128, attr));
  ASSERT_EQ(old_committed, quota.committed());
  for (int i = 0; i < 64; ++i) {
    ASSERT_EQ(static_cast<char>(0x3c), ptr[i]);
  }

  allocator.free(ptr);
  ASSERT_EQ(0, quota.committed());
  ASSERT_EQ(0, quota.reserved());
  ASSERT_EQ(0, tracker.used());
}

TEST(TestMemoryContextJemalloc, destroy_reclaims_owned_allocations)
{
  CountingAllocator backing_allocator;
  ObMemAttr attr("MemCtxTest");
  {
    MemoryContextMalloc owner(backing_allocator, attr, true);
    MemoryContextMalloc dispatcher(backing_allocator, attr, false);
    void *first = owner.alloc(64);
    void *second = owner.alloc(128);
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    ASSERT_EQ(2, backing_allocator.live_allocations());
    ASSERT_EQ(backing_allocator.live_bytes(), owner.total());

    dispatcher.free(first);
    ASSERT_EQ(1, backing_allocator.live_allocations());
    ASSERT_EQ(backing_allocator.live_bytes(), owner.total());
    ASSERT_EQ(0, dispatcher.total());

    const int64_t before_failed_realloc = owner.total();
    backing_allocator.fail_next_realloc();
    ASSERT_EQ(nullptr, dispatcher.realloc(second, 256, attr));
    ASSERT_EQ(before_failed_realloc, owner.total());
    ASSERT_EQ(1, backing_allocator.live_allocations());
    ASSERT_EQ(backing_allocator.live_bytes(), owner.total());

    second = dispatcher.realloc(second, 256, attr);
    ASSERT_NE(nullptr, second);
    ASSERT_EQ(1, backing_allocator.live_allocations());
    ASSERT_GT(owner.total(), 0);
    ASSERT_EQ(backing_allocator.live_bytes(), owner.total());
  }
  ASSERT_EQ(0, backing_allocator.live_allocations());
}

TEST(TestMemoryContextJemalloc, arena_usage_is_local_and_freeable_usage_is_quota_backed)
{
  ContextTestQuota tracker;
  ContextQuotaGuard tracker_guard(tracker);
  ContextParam param;
  param.set_mem_attr("TrackCtxArena", ObCtxIds::VECTOR_CTX_ID);
  MemoryContext context;
  ASSERT_EQ(OB_SUCCESS,
            MemoryContext::root()->CREATE_CONTEXT(context, param));
  const int64_t context_overhead = tracker.used();

  ASSERT_NE(nullptr, context->get_arena_allocator().alloc(4097));
  ASSERT_GT(context->arena_hold(), 0);
  ASSERT_EQ(context_overhead, tracker.used());

  context->reuse();
  ASSERT_EQ(0, context->arena_used());
  ASSERT_EQ(context_overhead, tracker.used());

  ASSERT_NE(nullptr, context->get_arena_allocator().alloc(4097));
  context->get_arena_allocator().reset();
  ASSERT_EQ(0, context->arena_hold());
  ASSERT_EQ(context_overhead, tracker.used());

  ASSERT_NE(nullptr, context->get_arena_allocator().alloc(4097));
  ASSERT_EQ(context_overhead, tracker.used());
  ASSERT_NE(nullptr, context->get_malloc_allocator().alloc(257));
  ASSERT_GT(context->malloc_hold(), 0);
  ASSERT_GT(tracker.used(), context_overhead);
  tracker.set_limit(tracker.used());
  EXPECT_EQ(nullptr, context->get_malloc_allocator().alloc(1));
  EXPECT_NE(nullptr, context->get_arena_allocator().alloc(2 * OB_MALLOC_MIDDLE_BLOCK_SIZE));
  DESTROY_CONTEXT(context);
  ASSERT_EQ(0, tracker.used());
}

TEST(TestMemoryContextParentDestroy, recursively_reclaims_children_once)
{
  ContextTestQuota tracker;
  ContextQuotaGuard tracker_guard(tracker);
  MemoryUsageTracker arena_tracker;
  ContextParam param;
  param.set_mem_attr("ParentDestroy", ObCtxIds::VECTOR_CTX_ID);
  MemoryContext parent;
  MemoryContext child;
  MemoryContext grandchild;
  ASSERT_EQ(OB_SUCCESS, MemoryContext::root()->CREATE_CONTEXT(parent, param));
  ASSERT_EQ(OB_SUCCESS, parent->CREATE_CONTEXT(child, param));
  ASSERT_EQ(OB_SUCCESS, child->CREATE_CONTEXT(grandchild, param));
  parent->get_arena_allocator().set_memory_tracker(&arena_tracker);
  grandchild->get_arena_allocator().set_memory_tracker(&arena_tracker);

  ASSERT_NE(nullptr, parent->get_arena_allocator().alloc(4097));
  ASSERT_NE(nullptr, child->get_malloc_allocator().alloc(257));
  ASSERT_NE(nullptr, grandchild->get_arena_allocator().alloc(8193));
  ASSERT_GT(tracker.used(), 0);
  ASSERT_GT(arena_tracker.used(), 0);

  DESTROY_CONTEXT(parent);
  parent = nullptr;
  child = nullptr;
  grandchild = nullptr;
  ASSERT_EQ(0, tracker.used());
  ASSERT_EQ(0, arena_tracker.used());
}

TEST(TestMemoryContextParentDestroy, explicitly_destroyed_child_is_unlinked)
{
  ContextTestQuota tracker;
  ContextQuotaGuard tracker_guard(tracker);
  ContextParam param;
  param.set_mem_attr("PartialDestroy", ObCtxIds::VECTOR_CTX_ID);
  MemoryContext parent;
  MemoryContext child;
  ASSERT_EQ(OB_SUCCESS, MemoryContext::root()->CREATE_CONTEXT(parent, param));
  ASSERT_EQ(OB_SUCCESS, parent->CREATE_CONTEXT(child, param));
  ASSERT_NE(nullptr, child->get_malloc_allocator().alloc(257));

  DESTROY_CONTEXT(child);
  child = nullptr;
  ASSERT_EQ(0, tracker.used());
  DESTROY_CONTEXT(parent);
  parent = nullptr;
  ASSERT_EQ(0, tracker.used());
}

TEST(TestMemoryContextParentDestroy, repeated_subtree_lifecycle)
{
  ContextTestQuota tracker;
  ContextQuotaGuard tracker_guard(tracker);
  MemoryUsageTracker arena_tracker;
  ContextParam param;
  param.set_mem_attr("RepeatedSubtree", ObCtxIds::VECTOR_CTX_ID);
  for (int i = 0; i < 1000; ++i) {
    MemoryContext parent;
    MemoryContext child;
    MemoryContext grandchild;
    ASSERT_EQ(OB_SUCCESS, MemoryContext::root()->CREATE_CONTEXT(parent, param));
    ASSERT_EQ(OB_SUCCESS, parent->CREATE_CONTEXT(child, param));
    ASSERT_EQ(OB_SUCCESS, child->CREATE_CONTEXT(grandchild, param));
    child->get_arena_allocator().set_memory_tracker(&arena_tracker);
    ASSERT_NE(nullptr, parent->get_malloc_allocator().alloc(17 + i % 31));
    ASSERT_NE(nullptr, child->get_arena_allocator().alloc(257 + i % 63));
    ASSERT_NE(nullptr, grandchild->get_malloc_allocator().alloc(65 + i % 17));
    DESTROY_CONTEXT(parent);
    parent = nullptr;
    child = nullptr;
    grandchild = nullptr;
    ASSERT_EQ(0, tracker.used());
    ASSERT_EQ(0, arena_tracker.used());
  }
}

#if defined(OB_HAVE_BUNDLED_JEMALLOC)
TEST(TestMallocBackend, aligned_jemalloc)
{
  void *ptr = jemalloc_memalign(64, 100);
  ASSERT_NE(nullptr, ptr);
  ASSERT_EQ(0U, reinterpret_cast<uintptr_t>(ptr) % 64);
  ob_free(ptr);
}

#if defined(__linux__) && !defined(ENABLE_SANITY)
TEST(TestMallocBackend, restore_after_fork)
{
  bool enabled = false;
  size_t enabled_size = sizeof(enabled);
  ASSERT_EQ(0, je_mallctl("background_thread", &enabled, &enabled_size,
                          nullptr, 0));
  ASSERT_TRUE(enabled);

  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (0 == pid) {
    enabled = true;
    enabled_size = sizeof(enabled);
    if (0 != je_mallctl("background_thread", &enabled, &enabled_size,
                        nullptr, 0) || enabled) {
      _exit(1);
    } else if (!restore_allocator_after_fork()) {
      _exit(2);
    }
    enabled = false;
    enabled_size = sizeof(enabled);
    if (0 != je_mallctl("background_thread", &enabled, &enabled_size,
                        nullptr, 0) || !enabled) {
      _exit(3);
    }
    _exit(0);
  }

  int status = 0;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));
}
#endif
#endif
