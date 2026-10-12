/*
 * Copyright (c) 2026 OceanBase.
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

#include <set>
#include <gtest/gtest.h>
#include "storage/ls/ob_ls_tablet_service.h"
#include "storage/multi_data_source/mds_table_mgr.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/multi_data_source/ob_mds_event_buffer.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::storage::mds;

TEST(MdsLifecycle, ResetPreservesOutstandingBorrowers)
{
  ObMdsTableMgr mgr;
  for (int cycle = 0; cycle < 2; ++cycle) {
    SCOPED_TRACE(cycle);
    MdsTableMgrHandle first;
    MdsTableMgrHandle second;
    ASSERT_EQ(OB_SUCCESS, first.set_mds_table_mgr(&mgr));
    ASSERT_EQ(OB_SUCCESS, second.set_mds_table_mgr(&mgr));
    EXPECT_EQ(2, mgr.get_ref());
    EXPECT_EQ(OB_EAGAIN, mgr.reset());
    EXPECT_EQ(2, mgr.get_ref());
    ASSERT_EQ(OB_SUCCESS, first.reset());
    EXPECT_EQ(OB_EAGAIN, mgr.reset());
    EXPECT_EQ(1, mgr.get_ref());
    ASSERT_EQ(OB_SUCCESS, second.reset());
    EXPECT_EQ(0, mgr.get_ref());
    EXPECT_EQ(OB_SUCCESS, mgr.reset());
  }
}

TEST(MdsLifecycle, TabletServiceWaitsForEveryEmbeddedManager)
{
  ObLSTabletService service;
  EXPECT_TRUE(service.is_safe_to_destroy());
  {
    MdsTableMgrHandle handle;
    ASSERT_EQ(OB_SUCCESS, service.get_mds_table_mgr(handle));
    EXPECT_FALSE(service.is_safe_to_destroy());
  }
  EXPECT_TRUE(service.is_safe_to_destroy());
  using Getter = int (ObLSTabletService::*)(ObMemtableMgrHandle &);
  Getter getters[] = {&ObLSTabletService::get_tx_data_memtable_mgr,
                      &ObLSTabletService::get_tx_ctx_memtable_mgr,
                      &ObLSTabletService::get_lock_memtable_mgr};
  for (Getter get : getters) {
    {
      ObMemtableMgrHandle handle;
      ASSERT_EQ(OB_SUCCESS, (service.*get)(handle));
      EXPECT_FALSE(service.is_safe_to_destroy());
    }
    EXPECT_TRUE(service.is_safe_to_destroy());
  }
  // The child cleanup contract is valid even before parent initialization.
  EXPECT_EQ(OB_SUCCESS, service.prepare_for_safe_destroy());
}

TEST(LsServiceLifecycle, InitializedWithoutStartCanStopAndDestroy)
{
  ObLSService service;
  for (int cycle = 0; cycle < 2; ++cycle) {
    SCOPED_TRACE(cycle);
    ASSERT_EQ(OB_SUCCESS, service.init());
    ASSERT_EQ(OB_SUCCESS, service.stop());
    ASSERT_EQ(OB_SUCCESS, service.wait());
    service.destroy();
  }
}


namespace
{
class EventBufferTestAllocator final : public ObIAllocator
{
public:
  ~EventBufferTestAllocator()
  {
    for (void *allocation : live_allocations_) {
      ob_free_align(allocation);
    }
  }
  void *alloc(const int64_t size) override
  {
    return alloc(size, ObMemAttr("TestMdsEvents"));
  }
  void *alloc(const int64_t size, const ObMemAttr &attr) override
  {
    void *allocation = nullptr;
    ++calls_;
    if (calls_ != fail_at_) {
      allocation = ob_malloc_align(64, size, attr);
      if (nullptr != allocation) { live_allocations_.insert(allocation); }
    }
    return allocation;
  }
  void free(void *allocation) override
  {
    if (nullptr != allocation) {
      EXPECT_EQ(1, live_allocations_.erase(allocation));
      ob_free_align(allocation);
    }
  }
  void fail_allocation(const int64_t ordinal) { calls_ = 0; fail_at_ = ordinal; }
  size_t live_allocations() const { return live_allocations_.size(); }
private:
  int64_t calls_ = 0;
  int64_t fail_at_ = -1;
  std::set<void *> live_allocations_;
};
using EventBuffer = cache::ObVtableEventRecycleBuffer<MdsEventKey, int64_t>;
}

TEST(MdsEventBufferLifecycle, ReinitializationAndRejectedInitPreserveValues)
{
  EventBufferTestAllocator allocator;
  EventBuffer buffer;
  const MdsEventKey key(ObTabletID(123));
  buffer.destroy();
  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE(cycle);
    ASSERT_EQ(OB_SUCCESS, buffer.init("TestMdsEvents", allocator, 2, 4096, 8));
    ASSERT_EQ(OB_SUCCESS, buffer.append(key, static_cast<int64_t>(42 + cycle)));
    EXPECT_EQ(OB_INIT_TWICE, buffer.init("TestMdsEvents", allocator, 2, 4096, 8));
    EXPECT_EQ(OB_INVALID_ARGUMENT, buffer.init("TestMdsEvents", allocator, 0, 4096, 8));
    int seen = 0;
    ASSERT_EQ(OB_SUCCESS, buffer.for_each(key, [&](const int64_t &value) {
      EXPECT_EQ(42 + cycle, value);
      ++seen;
      return OB_SUCCESS;
    }));
    EXPECT_EQ(1, seen);
    buffer.destroy();
    buffer.destroy();
    EXPECT_EQ(0, allocator.live_allocations());
    EXPECT_EQ(OB_NOT_INIT, buffer.append(key, static_cast<int64_t>(7)));
  }
}

TEST(MdsEventBufferLifecycle, FailedBucketAllocationsReleaseAllConstructedBuckets)
{
  EventBufferTestAllocator allocator;
  EventBuffer buffer;
  const MdsEventKey key(ObTabletID(456));
  // One allocation owns the wrapper array; one allocation per bucket owns
  // that bucket's real hash index and recycle buffer. Fail each boundary.
  for (int allocation = 1; allocation <= 4; ++allocation) {
    SCOPED_TRACE(allocation);
    allocator.fail_allocation(allocation);
    EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED,
        buffer.init("TestMdsEvents", allocator, 3, 4096, 8));
    EXPECT_EQ(0, allocator.live_allocations());
    EXPECT_EQ(OB_NOT_INIT, buffer.append(key, static_cast<int64_t>(99)));
    buffer.destroy();
    allocator.fail_allocation(-1);
    ASSERT_EQ(OB_SUCCESS, buffer.init("TestMdsEvents", allocator, 3, 4096, 8));
    ASSERT_EQ(OB_SUCCESS, buffer.append(key, static_cast<int64_t>(99)));
    int seen = 0;
    ASSERT_EQ(OB_SUCCESS, buffer.for_each(key, [&](const int64_t &value) {
      EXPECT_EQ(99, value);
      ++seen;
      return OB_SUCCESS;
    }));
    EXPECT_EQ(1, seen);
    buffer.destroy();
    EXPECT_EQ(0, allocator.live_allocations());
  }
}

TEST(MdsEventBufferLifecycle, DisabledSingletonCanInitializeAndDestroyRepeatedly)
{
  // The production singleton intentionally has zero enabled buckets. Its
  // member must still remain a live C++ object across every shutdown.
  for (int cycle = 0; cycle < 3; ++cycle) {
    ASSERT_EQ(OB_SUCCESS, ObMdsEventBuffer::init());
    ObMdsEventBuffer::destroy();
    ObMdsEventBuffer::destroy();
  }
}
