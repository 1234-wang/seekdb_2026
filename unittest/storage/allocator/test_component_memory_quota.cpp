/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#include "gtest/gtest.h"
#include <memory>
#include "lib/alloc/alloc_func.h"
#include "lib/allocator/ob_fifo_allocator.h"
#include "lib/rc/context.h"
#include "share/rc/ob_server_runtime.h"
#include "share/roaringbitmap/ob_rb_memory_mgr.h"
#include "storage/allocator/ob_vector_allocator.h"
#include "storage/allocator/ob_shared_memory_allocator_mgr.h"
#include "storage/meta_mem/ob_storage_meta_mem_mgr.h"
#include "storage/tablet/ob_tablet.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::storage;

namespace
{

MemoryQuota *test_vector_quota = nullptr;

MemoryQuota *resolve_test_vector_quota(const int64_t ctx_id)
{
  return ObCtxIds::VECTOR_CTX_ID == ctx_id ? test_vector_quota : nullptr;
}

class TestVectorQuotaResolverGuard final
{
public:
  explicit TestVectorQuotaResolverGuard(MemoryQuota &quota)
  {
    test_vector_quota = &quota;
    set_memory_quota_resolver(ObCtxIds::VECTOR_CTX_ID,
                              resolve_test_vector_quota);
  }
  ~TestVectorQuotaResolverGuard()
  {
    set_memory_quota_resolver(ObCtxIds::VECTOR_CTX_ID, nullptr);
    test_vector_quota = nullptr;
  }
};

class TestRbMemMgrGuard final
{
public:
  TestRbMemMgrGuard()
  {
    EXPECT_EQ(OB_SUCCESS, mem_mgr_.init());
    bind_server_service<ObRbMemMgr>(&mem_mgr_);
    EXPECT_EQ(OB_SUCCESS, ObRbMemMgr::init_memory_hook());
  }
  ~TestRbMemMgrGuard()
  {
    unbind_server_service<ObRbMemMgr>();
    mem_mgr_.destroy();
  }

  ObRbMemMgr &mem_mgr() { return mem_mgr_; }

private:
  ObRbMemMgr mem_mgr_;
};

class TestVectorAllocatorServiceGuard final
{
public:
  TestVectorAllocatorServiceGuard()
      : previous_(server_service<ObSharedMemAllocMgr>()),
        manager_(new ObSharedMemAllocMgr())
  {
    bind_server_service<ObSharedMemAllocMgr>(manager_.get());
  }
  ~TestVectorAllocatorServiceGuard()
  {
    manager_->vector_allocator().destroy();
    bind_server_service<ObSharedMemAllocMgr>(previous_);
  }
  ObVectorAllocator &allocator() { return manager_->vector_allocator(); }

private:
  ObSharedMemAllocMgr *previous_;
  std::unique_ptr<ObSharedMemAllocMgr> manager_;
};

struct TestMetaPoolObject
{
  void reset() {}
};

class TestMetaServiceGuard final
{
public:
  TestMetaServiceGuard()
      : previous_(server_service<ObStorageMetaMemMgr>()),
        manager_(nullptr)
  {
    EXPECT_EQ(OB_SUCCESS, ObStorageMetaMemMgr::server_module_new(manager_));
    bind_server_service<ObStorageMetaMemMgr>(manager_);
  }
  ~TestMetaServiceGuard()
  {
    bind_server_service<ObStorageMetaMemMgr>(previous_);
    if (nullptr != manager_) {
      OB_DELETE(ObStorageMetaMemMgr, ObMemAttr("MetaMemMgr"), manager_);
    }
  }
  bool valid() const { return nullptr != manager_; }
private:
  ObStorageMetaMemMgr *previous_;
  ObStorageMetaMemMgr *manager_;
};

} // namespace

TEST(TestVectorMemoryQuota, actual_bytes_and_dynamic_limit)
{
  ObVectorAllocator allocator;
  MemoryQuota &quota = allocator.memory_quota();
  quota.reset(128);
  ASSERT_TRUE(quota.reserve(64));
  ASSERT_TRUE(quota.reconcile(64, 80));
  EXPECT_EQ(80, quota.committed());
  quota.set_limit(40);
  EXPECT_FALSE(quota.reserve(1));
  quota.release(80);
  EXPECT_EQ(0, quota.committed());
}

TEST(TestVectorMemoryQuota, destroy_releases_context_pages_before_unregister)
{
  TestVectorAllocatorServiceGuard service_guard;
  ObVectorAllocator &allocator = service_guard.allocator();
  MemoryQuota &quota = allocator.memory_quota();

  for (int iteration = 0; iteration < 3; ++iteration) {
    ASSERT_EQ(OB_SUCCESS, allocator.init());
    ASSERT_EQ(&quota, resolve_memory_quota(ObCtxIds::VECTOR_CTX_ID));
    EXPECT_EQ(OB_INIT_TWICE, allocator.init());
    EXPECT_EQ(&quota, resolve_memory_quota(ObCtxIds::VECTOR_CTX_ID));
    quota.set_limit(4L << 20);
    ASSERT_NE(nullptr, allocator.get_mem_context()->get_arena_allocator().alloc(4097));
    ASSERT_NE(nullptr, allocator.alloc(257));
    ASSERT_GT(quota.committed(), 0);

    // Leave arena and freeable allocations alive: destroy must release both
    // while resolver-based page accounting can still find their owner.
    allocator.destroy();
    EXPECT_EQ(nullptr, resolve_memory_quota(ObCtxIds::VECTOR_CTX_ID));
    EXPECT_EQ(0, quota.committed());
    EXPECT_EQ(0, quota.reserved());
    allocator.destroy();
    EXPECT_EQ(nullptr, resolve_memory_quota(ObCtxIds::VECTOR_CTX_ID));
  }
}

TEST(TestVectorMemoryQuota, production_bitmap_path_charges_and_releases_quota)
{
  ObVectorAllocator allocator;
  MemoryQuota &quota = allocator.memory_quota();
  quota.reset(1L << 20);
  TestVectorQuotaResolverGuard resolver_guard(quota);
  TestRbMemMgrGuard rb_mem_mgr_guard;

  roaring_bitmap_t *bitmap = nullptr;
  {
    oceanbase::lib::ObMallocHookAttrGuard attr_guard(
        oceanbase::lib::ObMemAttr("VIBQuotaTest"));
    bitmap = roaring_bitmap_create();
    ASSERT_NE(nullptr, bitmap);
    for (uint32_t value = 0; value < 4096; value += 2) {
      roaring_bitmap_add(bitmap, value);
    }
  }

  const int64_t actual_charge = quota.committed();
  EXPECT_EQ(2048, roaring_bitmap_get_cardinality(bitmap));
  EXPECT_GT(actual_charge, 0);
  EXPECT_EQ(actual_charge, rb_mem_mgr_guard.mem_mgr().get_vec_idx_used());
  EXPECT_EQ(0, quota.reserved());

  roaring_bitmap_free(bitmap);
  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reserved());
  EXPECT_EQ(0, rb_mem_mgr_guard.mem_mgr().get_vec_idx_used());

  quota.set_limit(1);
  {
    oceanbase::lib::ObMallocHookAttrGuard attr_guard(
        oceanbase::lib::ObMemAttr("VIBQuotaTest"));
    EXPECT_THROW(roaring_bitmap_create(), std::bad_alloc);
  }
  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reserved());
  EXPECT_EQ(0, rb_mem_mgr_guard.mem_mgr().get_vec_idx_used());
}

TEST(TestVectorMemoryQuota, fifo_metadata_pages_do_not_use_vector_quota)
{
  MemoryQuota quota(0);
  TestVectorQuotaResolverGuard resolver_guard(quota);
  {
    ObFIFOAllocator allocator;
    ASSERT_EQ(OB_SUCCESS, allocator.init(
        nullptr, OB_MALLOC_MIDDLE_BLOCK_SIZE, ObMemAttr("VecIdxSrv")));
    void *metadata = allocator.alloc(128);
    ASSERT_NE(nullptr, metadata);
    EXPECT_EQ(128, allocator.used());
    EXPECT_GT(allocator.total(), 0);
    EXPECT_EQ(0, quota.committed());
    EXPECT_EQ(0, quota.reserved());
    allocator.free(metadata);
    EXPECT_EQ(0, allocator.used());
    EXPECT_GT(allocator.total(), 0); // The empty current page remains reusable.
    allocator.reset();
    EXPECT_EQ(0, allocator.total());
    EXPECT_EQ(0, quota.committed());
    EXPECT_EQ(0, quota.reserved());
  }
}

TEST(TestMetaMemoryAccounting, inner_pages_overflow_and_free_list_are_counted)
{
  EXPECT_EQ(nullptr, resolve_memory_quota(ObCtxIds::META_OBJ_CTX_ID));
  MemoryUsageTracker tracker;
  {
    ObStorageMetaObjPool<TestMetaPoolObject> pool(
        1, "MetaAccounting", ObCtxIds::META_OBJ_CTX_ID, tracker);
    const int64_t construction_charge = tracker.used();
    EXPECT_GT(construction_charge, 0);

    TestMetaPoolObject *inner_object = nullptr;
    ASSERT_EQ(OB_SUCCESS, pool.acquire(inner_object));
    ASSERT_NE(nullptr, inner_object);
    const int64_t inner_charge = tracker.used();
    EXPECT_GT(inner_charge, construction_charge);

    TestMetaPoolObject *overflow_object = nullptr;
    ASSERT_EQ(OB_SUCCESS, pool.acquire(overflow_object));
    ASSERT_NE(nullptr, overflow_object);
    const int64_t overflow_charge = tracker.used();
    EXPECT_GT(overflow_charge, inner_charge);

    pool.release(overflow_object);
    EXPECT_EQ(inner_charge, tracker.used());
    pool.release(inner_object);
    EXPECT_EQ(inner_charge, tracker.used()); // Cached ordinary node stays charged.

    ASSERT_EQ(OB_SUCCESS, pool.acquire(inner_object));
    EXPECT_EQ(inner_charge, tracker.used()); // Reuse must not charge twice.
    pool.release(inner_object);

    pool.destroy();
    EXPECT_EQ(0, tracker.used());
    pool.destroy();
    EXPECT_EQ(0, tracker.used());
  }
  EXPECT_EQ(0, tracker.used());
}

TEST(TestMetaMemoryAccounting, full_tablet_creator_counts_and_releases_backing)
{
  TestMetaServiceGuard service_guard;
  ASSERT_TRUE(service_guard.valid());
  ObFullTabletCreator creator;
  ASSERT_EQ(OB_SUCCESS, creator.init());
  EXPECT_EQ(nullptr, resolve_memory_quota(ObCtxIds::META_OBJ_CTX_ID));
  EXPECT_EQ(0, creator.get_memory_used());

  ObTabletHandle handle;
  ASSERT_EQ(OB_SUCCESS, creator.create_tablet(handle));
  ObTablet *tablet = handle.get_obj();
  ASSERT_NE(nullptr, tablet);
  EXPECT_GT(creator.get_memory_used(), 0);
  // Keep a reference across handle reset, so this isolated creator test does
  // not enqueue the tablet into the production T3M GC queue.
  tablet->inc_ref();
  handle.reset();
  EXPECT_EQ(0, tablet->dec_ref());
  creator.free_tablet(tablet);
  EXPECT_GT(creator.get_memory_used(), 0); // Tiny FIFO retains its current page.
  creator.reset();
  EXPECT_EQ(0, creator.get_memory_used());
}

TEST(TestVectorMemoryQuota, fifo_does_not_resolve_context_quota)
{
  MemoryQuota quota(0);
  TestVectorQuotaResolverGuard resolver_guard(quota);
  ObFIFOAllocator allocator;
  ASSERT_EQ(OB_SUCCESS, allocator.init(
      nullptr, OB_MALLOC_MIDDLE_BLOCK_SIZE, ObMemAttr("FifoNoQuota", ObCtxIds::VECTOR_CTX_ID)));
  void *normal = allocator.alloc(128);
  void *special = allocator.alloc(2 * OB_MALLOC_MIDDLE_BLOCK_SIZE);
  ASSERT_NE(nullptr, normal);
  ASSERT_NE(nullptr, special);
  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reject_count());
  allocator.free(normal);
  allocator.free(special);
  EXPECT_EQ(0, allocator.used());
  allocator.reset();
  EXPECT_EQ(0, allocator.total());
}
