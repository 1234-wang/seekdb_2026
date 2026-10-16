/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#include <atomic>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/resource/ob_memory_quota.h"
#include "observer/virtual_table/ob_all_virtual_resource_limit.h"
#include "share/cache/ob_kv_storecache.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/ob_sql_memory_manager.h"
#include "storage/allocator/ob_shared_memory_allocator_mgr.h"
#include "storage/meta_mem/ob_storage_meta_mem_mgr.h"

using oceanbase::common::MemoryQuota;
using oceanbase::common::MemoryQuotaSample;
using oceanbase::common::MemoryReservation;
using oceanbase::observer::ObResourceLimitTable;

namespace
{

using StorageMetaMemMgr = oceanbase::storage::ObStorageMetaMemMgr;

class ComponentServiceBindings final
{
public:
  ComponentServiceBindings()
      : meta_manager_(nullptr), kv_cache_initialized_(false)
  {}

  int init()
  {
    int ret = oceanbase::storage::ObStorageMetaMemMgr::server_module_new(
        meta_manager_);
    if (oceanbase::common::OB_SUCCESS == ret) {
      oceanbase::share::bind_server_service<oceanbase::sql::ObSqlMemoryManager>(
          &sql_manager_);
      oceanbase::share::bind_server_service<oceanbase::share::ObSharedMemAllocMgr>(
          &shared_manager_);
      oceanbase::share::bind_server_service<oceanbase::storage::ObStorageMetaMemMgr>(
          meta_manager_);
      ret = oceanbase::common::ObKVGlobalCache::get_instance().init(
          53, 8 * BLOCK_SIZE, BLOCK_SIZE, 0,
          oceanbase::common::ObKVCacheRuntimeOptions(1000000, 5 * BLOCK_SIZE));
      kv_cache_initialized_ = oceanbase::common::OB_SUCCESS == ret;
    }
    return ret;
  }

  ~ComponentServiceBindings()
  {
    if (kv_cache_initialized_) {
      oceanbase::common::ObKVGlobalCache::get_instance().stop();
      oceanbase::common::ObKVGlobalCache::get_instance().wait();
      oceanbase::common::ObKVGlobalCache::get_instance().destroy();
    }
    oceanbase::share::unbind_server_service<oceanbase::storage::ObStorageMetaMemMgr>();
    oceanbase::share::unbind_server_service<oceanbase::share::ObSharedMemAllocMgr>();
    oceanbase::share::unbind_server_service<oceanbase::sql::ObSqlMemoryManager>();
    if (nullptr != meta_manager_) {
      OB_DELETE(StorageMetaMemMgr,
                oceanbase::common::ObMemAttr("MetaMemMgr"), meta_manager_);
    }
  }

  oceanbase::sql::ObSqlMemoryManager &sql_manager() { return sql_manager_; }
  oceanbase::share::ObSharedMemAllocMgr &shared_manager() { return shared_manager_; }
  oceanbase::storage::ObStorageMetaMemMgr &meta_manager() { return *meta_manager_; }

  static const int64_t BLOCK_SIZE = 64L << 10;

private:
  oceanbase::sql::ObSqlMemoryManager sql_manager_;
  oceanbase::share::ObSharedMemAllocMgr shared_manager_;
  oceanbase::storage::ObStorageMetaMemMgr *meta_manager_;
  bool kv_cache_initialized_;
};

} // namespace

TEST(TestResourceLimitMemory, stable_resource_keys)
{
  ASSERT_EQ(4, ObResourceLimitTable::memory_resource_count());
  EXPECT_STREQ("kv_cache_memory", ObResourceLimitTable::memory_resource_name(0));
  EXPECT_STREQ("sql_workarea_memory", ObResourceLimitTable::memory_resource_name(1));
  EXPECT_STREQ("vector_memory", ObResourceLimitTable::memory_resource_name(2));
  EXPECT_STREQ("meta_object_memory", ObResourceLimitTable::memory_resource_name(3));
  EXPECT_EQ(nullptr, ObResourceLimitTable::memory_resource_name(-1));
  EXPECT_EQ(nullptr, ObResourceLimitTable::memory_resource_name(4));
  MemoryQuotaSample sample;
  EXPECT_EQ(oceanbase::common::OB_INVALID_ARGUMENT,
            ObResourceLimitTable::get_memory_resource_sample(-1, sample));
  EXPECT_EQ(oceanbase::common::OB_INVALID_ARGUMENT,
            ObResourceLimitTable::get_memory_resource_sample(4, sample));
}

TEST(TestResourceLimitMemory, approximate_sample_is_safe_during_writes)
{
  static const int WRITER_COUNT = 8;
  static const int ITERATIONS = 20000;
  MemoryQuota quota(WRITER_COUNT);
  std::atomic<bool> start(false);
  std::atomic<bool> failed(false);
  std::vector<std::thread> writers;
  writers.reserve(WRITER_COUNT);

  for (int i = 0; i < WRITER_COUNT; ++i) {
    writers.emplace_back([&]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int j = 0; j < ITERATIONS; ++j) {
        MemoryReservation reservation(&quota, 1);
        if (reservation.valid() && reservation.reconcile(1)) {
          quota.release(1);
        }
      }
    });
  }

  start.store(true, std::memory_order_release);
  int64_t previous_rejects = 0;
  int64_t previous_reclaims = 0;
  for (int i = 0; i < ITERATIONS; ++i) {
    const MemoryQuotaSample sample = quota.sample();
    if (sample.limit_bytes_ < 0 || sample.committed_bytes_ < 0
        || sample.reserved_bytes_ < 0
        || sample.reject_count_ < previous_rejects
        || sample.reclaim_count_ < previous_reclaims) {
      failed.store(true, std::memory_order_release);
    }
    previous_rejects = sample.reject_count_;
    previous_reclaims = sample.reclaim_count_;
  }

  for (std::thread &writer : writers) {
    writer.join();
  }
  EXPECT_FALSE(failed.load(std::memory_order_acquire));
  const MemoryQuotaSample quiescent = quota.sample();
  EXPECT_EQ(0, quiescent.reserved_bytes_);
  EXPECT_EQ(0, quiescent.committed_bytes_);
}

TEST(TestResourceLimitMemory, production_manager_routing)
{
  ComponentServiceBindings services;
  ASSERT_EQ(oceanbase::common::OB_SUCCESS, services.init());

  services.sql_manager().get_workarea_quota().set_limit(11 * 1024 * 1024);
  services.shared_manager().vector_allocator().memory_quota().set_limit(
      12 * 1024 * 1024);
  services.meta_manager().get_memory_quota().set_limit(13 * 1024 * 1024);

  static const int64_t EXPECTED_LIMITS[] = {
      5 * ComponentServiceBindings::BLOCK_SIZE,
      11 * 1024 * 1024,
      12 * 1024 * 1024,
      13 * 1024 * 1024,
  };
  for (int64_t i = 0; i < ObResourceLimitTable::memory_resource_count(); ++i) {
    MemoryQuotaSample sample;
    ASSERT_EQ(oceanbase::common::OB_SUCCESS,
              ObResourceLimitTable::get_memory_resource_sample(i, sample));
    EXPECT_EQ(EXPECTED_LIMITS[i], sample.limit_bytes_);
  }

  // Distinguish physically committed bytes from an in-flight reservation.
  MemoryQuota &quota = services.sql_manager().get_workarea_quota();
  MemoryReservation allocation(&quota, 128);
  ASSERT_TRUE(allocation.valid());
  ASSERT_TRUE(allocation.reconcile(96));
  {
    MemoryReservation pending(&quota, 64);
    ASSERT_TRUE(pending.valid());
    MemoryQuotaSample sample;
    ASSERT_EQ(oceanbase::common::OB_SUCCESS,
              ObResourceLimitTable::get_memory_resource_sample(1, sample));
    EXPECT_EQ(96, sample.committed_bytes_);
    EXPECT_EQ(64, sample.reserved_bytes_);
    quota.set_limit(32);
    ASSERT_EQ(oceanbase::common::OB_SUCCESS,
              ObResourceLimitTable::get_memory_resource_sample(1, sample));
    EXPECT_EQ(32, sample.limit_bytes_);
    EXPECT_EQ(96, sample.committed_bytes_);
  }
  quota.release(96);
  MemoryQuotaSample quiescent;
  ASSERT_EQ(oceanbase::common::OB_SUCCESS,
            ObResourceLimitTable::get_memory_resource_sample(1, quiescent));
  EXPECT_EQ(0, quiescent.committed_bytes_);
  EXPECT_EQ(0, quiescent.reserved_bytes_);
}

TEST(TestResourceLimitMemory, missing_service_is_not_zero_usage)
{
  MemoryQuotaSample sample;
  EXPECT_EQ(oceanbase::common::OB_NOT_INIT,
            ObResourceLimitTable::get_memory_resource_sample(1, sample));
  EXPECT_EQ(oceanbase::common::OB_NOT_INIT,
            ObResourceLimitTable::get_memory_resource_sample(2, sample));
  EXPECT_EQ(oceanbase::common::OB_NOT_INIT,
            ObResourceLimitTable::get_memory_resource_sample(3, sample));
}
