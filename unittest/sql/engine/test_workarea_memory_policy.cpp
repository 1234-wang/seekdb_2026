/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#include "gtest/gtest.h"
#include "lib/alloc/alloc_func.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/allocator/page_arena.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "sql/engine/basic/ob_chunk_row_store.h"
#include "sql/engine/basic/ob_ra_datum_store.h"
#include "sql/engine/basic/ob_ra_row_store.h"
#include "sql/engine/basic/ob_workarea_memory_limit.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_sql_memory_manager.h"
#include "sql/engine/ob_sql_mem_mgr_processor.h"
#include "sql/dtl/ob_dtl_interm_result_manager.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/tmp_file/ob_tmp_file_manager.h"

#include <algorithm>
#include <vector>
#include <thread>

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{

constexpr int64_t TEST_MAX_BATCH_SIZE = 1024;

class TestTmpFileManagerGuard final
{
public:
  TestTmpFileManagerGuard() : started_(false), bound_(false) {}

  int init()
  {
    int ret = manager_.init();
    if (OB_SUCCESS == ret) {
      ret = manager_.start();
      started_ = OB_SUCCESS == ret;
    }
    if (OB_SUCCESS == ret) {
      oceanbase::share::bind_server_service<oceanbase::tmp_file::ObTmpFileManager>(
          &manager_);
      bound_ = true;
    }
    return ret;
  }

  ~TestTmpFileManagerGuard()
  {
    if (bound_) {
      oceanbase::share::unbind_server_service<oceanbase::tmp_file::ObTmpFileManager>();
    }
    if (started_) {
      manager_.stop();
      manager_.wait();
    }
    manager_.destroy();
  }

private:
  oceanbase::tmp_file::ObTmpFileManager manager_;
  bool started_;
  bool bound_;
};

class TestSqlMemoryManagerGuard final
{
public:
  TestSqlMemoryManagerGuard() : manager_(nullptr), bound_(false) {}

  int init()
  {
    int ret = ObSqlMemoryManager::server_module_new(manager_);
    if (OB_SUCCESS == ret) {
      ret = ObSqlMemoryManager::server_module_init(manager_);
    }
    if (OB_SUCCESS == ret) {
      oceanbase::share::bind_server_service<ObSqlMemoryManager>(manager_);
      bound_ = true;
    }
    return ret;
  }

  ~TestSqlMemoryManagerGuard()
  {
    if (bound_) {
      oceanbase::share::unbind_server_service<ObSqlMemoryManager>();
    }
    ObSqlMemoryManager::server_module_destroy(manager_);
  }

  ObSqlMemoryManager &get() { return *manager_; }

private:
  ObSqlMemoryManager *manager_;
  bool bound_;
};

class TestStoredRow final
{
public:
  explicit TestStoredRow(const int64_t row_size)
      : bytes_(row_size, 0),
        row_(new (bytes_.data()) ObChunkDatumStore::StoredRow())
  {
    row_->cnt_ = 0;
    row_->row_size_ = static_cast<uint32_t>(row_size);
  }

  const ObChunkDatumStore::StoredRow &get() const { return *row_; }

private:
  std::vector<char> bytes_;
  ObChunkDatumStore::StoredRow *row_;
};

// Keep the local spill policy test inside the SQL module.  The temporary-file
// service has its own module tests; here a successful write is enough to drive
// the real ObChunkDatumStore dump, block reuse and callback paths.
class TestChunkDatumStore final : public ObChunkDatumStore
{
public:
  TestChunkDatumStore(const oceanbase::lib::ObLabel &label,
                      ObIAllocator &allocator,
                      ObSqlMemoryCallback &callback)
      : ObChunkDatumStore(label, &allocator),
        callback_(callback),
        spilled_bytes_(0)
  {}

  int64_t spilled_bytes() const { return spilled_bytes_; }

private:
  int write_file(void *buf, const int64_t size) override
  {
    int ret = OB_SUCCESS;
    if (size < 0 || (size > 0 && nullptr == buf)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (size > 0) {
      spilled_bytes_ += size;
      callback_.dumped(size);
    }
    return ret;
  }

  ObSqlMemoryCallback &callback_;
  int64_t spilled_bytes_;
};

class DefaultBackedChunkDatumStore final : public ObChunkDatumStore
{
public:
  explicit DefaultBackedChunkDatumStore(const oceanbase::lib::ObLabel &label)
      : ObChunkDatumStore(label), spilled_bytes_(0)
  {}

  int64_t spilled_bytes() const { return spilled_bytes_; }

private:
  int write_file(void *buf, const int64_t size) override
  {
    int ret = OB_SUCCESS;
    if (size < 0 || (size > 0 && nullptr == buf)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      spilled_bytes_ += size;
    }
    return ret;
  }

  int64_t spilled_bytes_;
};

class FailingAllocator final : public ObIAllocator
{
public:
  explicit FailingAllocator(ObIAllocator &allocator)
      : allocator_(allocator), fail_(false), failures_(0) {}
  void arm() { fail_ = true; }
  int failures() const { return failures_; }
  void *alloc(const int64_t size) override { return alloc(size, ObMemAttr()); }
  void *alloc(const int64_t size, const ObMemAttr &attr) override
  {
    if (fail_) {
      fail_ = false;
      ++failures_;
      return nullptr;
    }
    return allocator_.alloc(size, attr);
  }
  void free(void *ptr) override { allocator_.free(ptr); }
private:
  ObIAllocator &allocator_;
  bool fail_;
  int failures_;
};

std::string batch_value(const int64_t index)
{
  return "datum-" + std::to_string(index) + "-payload";
}

int add_string_vectorized_batch(ObChunkDatumStore &store,
                                ObIAllocator &allocator,
                                const int64_t batch_size,
                                std::vector<ObChunkDatumStore::StoredRow *> &rows)
{
  int ret = OB_SUCCESS;
  ObExecContext exec_ctx(allocator);
  ObEvalCtx eval_ctx(exec_ctx);
  // BatchCtx is allocated once per store using the query's maximum batch
  // capacity, not the number of rows in its first (possibly partial) batch.
  eval_ctx.set_max_batch_size(TEST_MAX_BATCH_SIZE);

  ObExpr expr;
  expr.datum_meta_.type_ = ObVarcharType;
  expr.batch_result_ = true;
  expr.frame_idx_ = 0;
  expr.datum_off_ = 0;
  expr.eval_info_off_ = sizeof(ObDatum) * TEST_MAX_BATCH_SIZE;
  const int64_t frame_size = expr.eval_info_off_ + sizeof(ObEvalInfo);
  std::vector<uint64_t> frame_words(
      (frame_size + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
  char *frame = reinterpret_cast<char *>(frame_words.data());
  std::vector<std::string> payloads(batch_size);
  for (int64_t i = 0; i < batch_size; ++i) {
    payloads[i] = batch_value(i);
    ObDatum *datum = new (frame + sizeof(ObDatum) * i) ObDatum();
    datum->set_string(payloads[i].data(), payloads[i].size());
  }
  ObEvalInfo *eval_info = new (frame + expr.eval_info_off_) ObEvalInfo();
  eval_info->projected_ = true;
  char *frames[] = {frame};
  exec_ctx.set_frames(frames);
  eval_ctx.frames_ = frames;
  ObSEArray<ObExpr *, 1> exprs;
  if (OB_FAIL(exprs.push_back(&expr))) {
    return ret;
  }
  std::vector<char> skip_mem(ObBitVector::memory_size(batch_size), 0);
  ObBitVector *skip = to_bit_vector(skip_mem.data());
  skip->reset(batch_size);
  std::vector<uint16_t> selector(batch_size);
  rows.resize(batch_size, nullptr);
  for (int64_t i = 0; i < batch_size; ++i) {
    selector[i] = static_cast<uint16_t>(i);
  }
  ret = store.add_batch(exprs, eval_ctx, *skip, batch_size,
                        selector.data(), batch_size, rows.data());
  return ret;
}

class TestRADatumStore final : public ObRADatumStore
{
public:
  TestRADatumStore() : ObRADatumStore(), spilled_bytes_(0) {}

  TestRADatumStore(ObIAllocator &allocator, ObSqlMemoryCallback &)
      : ObRADatumStore(&allocator), spilled_bytes_(0)
  {}

  int64_t spilled_bytes() const { return spilled_bytes_; }

private:
  int write_file(BlockIndex &bi, void *buf, int64_t size) override
  {
    const int ret = ObRADatumStore::write_file(bi, buf, size);
    if (OB_SUCCESS == ret) {
      spilled_bytes_ += size;
    }
    return ret;
  }

  int64_t spilled_bytes_;
};

class TestChunkRowStore final : public ObChunkRowStore
{
public:
  TestChunkRowStore(ObIAllocator &allocator, ObSqlMemoryCallback &)
      : ObChunkRowStore(&allocator), spilled_bytes_(0)
  {}

  int64_t spilled_bytes() const { return spilled_bytes_; }

private:
  int write_file(void *buf, int64_t size) override
  {
    const int ret = ObChunkRowStore::write_file(buf, size);
    if (OB_SUCCESS == ret) {
      spilled_bytes_ += size;
    }
    return ret;
  }

  int64_t spilled_bytes_;
};

} // namespace


TEST(TestWorkareaMemoryPolicy, explicit_local_limits_and_overflow)
{
  EXPECT_FALSE(should_spill_workarea(0, INT64_MAX, INT64_MAX));
  EXPECT_FALSE(should_spill_workarea(100, 80, 20));
  EXPECT_TRUE(should_spill_workarea(100, 80, 21));
  EXPECT_FALSE(should_spill_workarea(INT64_MAX, INT64_MAX - 1, 1));
  EXPECT_TRUE(should_spill_workarea(INT64_MAX, INT64_MAX, 1));
}

TEST(TestWorkareaMemoryPolicy, chunk_datum_store_uses_explicit_allocator)
{
  DefaultPageAllocator backing(ObMemAttr("WAExternal", ObCtxIds::WORK_AREA));
  const TestStoredRow row(128);
  for (const bool use_setter : {false, true}) {
    FailingAllocator allocator(backing);
    ObChunkDatumStore store("WAExternal", use_setter ? nullptr : &allocator);
    if (use_setter) {
      store.set_allocator(allocator);
    }
    ASSERT_EQ(OB_SUCCESS, store.init(0, ObCtxIds::WORK_AREA, "WAExternal", false,
        0, ObChunkDatumStore::MIN_BLOCK_SIZE));
    allocator.arm();
    EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, store.add_row(row.get()));
    EXPECT_EQ(1, allocator.failures());
    ASSERT_EQ(OB_SUCCESS, store.add_row(row.get()));
    EXPECT_EQ(1, store.get_row_cnt());
    store.reset();
  }
}

TEST(TestWorkareaMemoryPolicy, ra_datum_store_uses_explicit_allocator)
{
  DefaultPageAllocator backing(ObMemAttr("WAExternal", ObCtxIds::WORK_AREA));
  const int64_t value = 42;
  ObDatum datum(reinterpret_cast<const char *>(&value), sizeof(value), false);
  ObSEArray<ObDatum, 1> row;
  ASSERT_EQ(OB_SUCCESS, row.push_back(datum));
  for (const bool use_setter : {false, true}) {
    FailingAllocator allocator(backing);
    ObRADatumStore store(use_setter ? nullptr : &allocator);
    if (use_setter) {
      store.set_allocator(allocator);
    }
    ASSERT_EQ(OB_SUCCESS, store.init(0, ObCtxIds::WORK_AREA, "WAExternal"));
    allocator.arm();
    EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, store.add_row(row));
    EXPECT_EQ(1, allocator.failures());
    ASSERT_EQ(OB_SUCCESS, store.add_row(row));
    ASSERT_EQ(OB_SUCCESS, store.finish_add_row());
    EXPECT_EQ(1, store.get_row_cnt());
    const ObRADatumStore::StoredRow *stored_row = nullptr;
    ASSERT_EQ(OB_SUCCESS, store.get_row(0, stored_row));
    ASSERT_NE(nullptr, stored_row);
    EXPECT_EQ(42, stored_row->cells()[0].get_int());
    store.reset();
  }
}

TEST(TestWorkareaMemoryPolicy, ra_row_store_random_access_and_error_codes)
{
  ObRARowStore store;
  ObObj cell;
  ObNewRow row;
  row.cells_ = &cell;
  row.count_ = 1;
  EXPECT_EQ(OB_NOT_INIT, store.add_row(row));
  ASSERT_EQ(OB_SUCCESS, store.init(0, ObCtxIds::WORK_AREA, "WARARow"));
  EXPECT_EQ(OB_INIT_TWICE, store.init(0));
  std::vector<char> payload(40L << 10);
  for (int64_t i = 0; i < 8; ++i) {
    std::fill(payload.begin(), payload.end(), 'a' + i);
    cell.set_varchar(ObString(payload.size(), payload.data()));
    ASSERT_EQ(OB_SUCCESS, store.add_row(row));
  }
  ASSERT_EQ(OB_SUCCESS, store.finish_add_row());
  EXPECT_FALSE(store.is_file_open());
  const ObNewRow *stored_row = nullptr;
  for (const int64_t row_id : {7, 0, 4, 2, 6, 1, 5, 3}) {
    ASSERT_EQ(OB_SUCCESS, store.get_row(row_id, stored_row));
    ASSERT_NE(nullptr, stored_row);
    std::fill(payload.begin(), payload.end(), 'a' + row_id);
    EXPECT_EQ(ObString(payload.size(), payload.data()),
              stored_row->cells_[0].get_varchar());
  }
  EXPECT_EQ(OB_INDEX_OUT_OF_RANGE, store.get_row(-1, stored_row));
  EXPECT_EQ(OB_INDEX_OUT_OF_RANGE, store.get_row(8, stored_row));
}

TEST(TestWorkareaMemoryPolicy, budget_is_not_allocation_admission)
{
  TestSqlMemoryManagerGuard guard;
  ASSERT_EQ(OB_SUCCESS, guard.init());
  auto &manager = guard.get();
  ASSERT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(false, 1, nullptr, false));
  EXPECT_EQ(1, manager.get_max_workarea_size());
  EXPECT_EQ(nullptr, resolve_memory_quota(ObCtxIds::WORK_AREA));
  DefaultBackedChunkDatumStore store("NoWAQuota");
  ASSERT_EQ(OB_SUCCESS, store.init(0, ObCtxIds::WORK_AREA, "NoWAQuota", true,
                                  0, ObChunkDatumStore::MIN_BLOCK_SIZE));
  const TestStoredRow row(3L << 10);
  for (int i = 0; i < 20; ++i) {
    ASSERT_EQ(OB_SUCCESS, store.add_row(row.get()));
  }
  EXPECT_GT(store.get_mem_hold(), manager.get_max_workarea_size());
  EXPECT_EQ(0, store.spilled_bytes());
  EXPECT_EQ(0, manager.get_total_mem_used()); // Allocations alone are not profiles.
  ASSERT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(true, 8L << 20, nullptr, false));
  EXPECT_EQ(8L << 20, manager.get_max_auto_workarea_size());
  ASSERT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(false, 4L << 20, nullptr, false));
  EXPECT_FALSE(manager.enable_auto_memory_mgr());
  EXPECT_EQ(4L << 20, manager.get_max_workarea_size());
}

TEST(TestWorkareaMemoryPolicy, registered_profile_reports_follow_lifecycle_and_policy_switch)
{
  TestSqlMemoryManagerGuard guard;
  ASSERT_EQ(OB_SUCCESS, guard.init());
  auto &manager = guard.get();
  ObSqlWorkAreaProfile profile(ObSqlWorkAreaType::SORT_WORK_AREA);
  profile.init(32L << 20, OB_MALLOC_MIDDLE_BLOCK_SIZE);
  ObSqlMemMgrProcessor processor(profile);
  processor.set_sql_mem_mgr(&manager);
  processor.alloc(100);
  EXPECT_EQ(0, manager.get_total_mem_used());
  ASSERT_EQ(OB_SUCCESS, manager.register_work_area_profile(profile));
  ASSERT_TRUE(profile.is_registered());
  EXPECT_EQ(100, manager.get_total_mem_used());
  processor.alloc(200);
  EXPECT_EQ(300, manager.get_total_mem_used());
  ASSERT_EQ(OB_SUCCESS, processor.update_used_mem_size(400));
  EXPECT_EQ(400, manager.get_total_mem_used());
  processor.free(100);
  EXPECT_EQ(300, manager.get_total_mem_used());
  ObSqlWorkareaCurrentMemoryInfo info;
  ASSERT_EQ(OB_SUCCESS, manager.get_workarea_memory_info(info));
  EXPECT_EQ(300, info.get_total_mem_used());
  ASSERT_EQ(OB_SUCCESS, manager.unregister_work_area_profile(profile));
  EXPECT_EQ(0, manager.get_total_mem_used());
  processor.free(200);
  EXPECT_EQ(0, manager.get_total_mem_used());

  // Entering AUTO resets membership, not the operator's own reported usage.
  ASSERT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(true, 8L << 20, nullptr, false));
  ASSERT_EQ(OB_SUCCESS, manager.register_work_area_profile(profile));
  EXPECT_EQ(100, manager.get_total_mem_used());
  ASSERT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(false, 4L << 20, nullptr, false));
  EXPECT_FALSE(profile.is_registered());
  EXPECT_EQ(0, manager.get_total_mem_used());
  processor.alloc(10);
  EXPECT_EQ(0, manager.get_total_mem_used());
  ASSERT_EQ(OB_SUCCESS, manager.register_work_area_profile(profile));
  EXPECT_EQ(110, manager.get_total_mem_used());
  ASSERT_EQ(OB_SUCCESS, manager.unregister_work_area_profile(profile));
  EXPECT_EQ(0, manager.get_total_mem_used());
}

TEST(TestWorkareaMemoryPolicy, spill_callback_records_bytes_not_admission)
{
  ObSqlMemoryTracker tracker;
  tracker.dumped(256);
  EXPECT_EQ(256, tracker.get_total_dump_size());
  tracker.dumped(-256);
  EXPECT_EQ(0, tracker.get_total_dump_size());
}

TEST(TestWorkareaMemoryPolicy, reporting_concurrent_with_policy_reset_has_no_residual)
{
  TestSqlMemoryManagerGuard guard;
  ASSERT_EQ(OB_SUCCESS, guard.init());
  auto &manager = guard.get();
  ObSqlWorkAreaProfile profile(ObSqlWorkAreaType::SORT_WORK_AREA);
  profile.init(32L << 20, OB_MALLOC_MIDDLE_BLOCK_SIZE);
  ObSqlMemMgrProcessor processor(profile);
  processor.set_sql_mem_mgr(&manager);
  std::thread worker([&]() {
    for (int i = 0; i < 10000; ++i) {
      processor.alloc(64);
      processor.free(64);
    }
  });
  // Do not ASSERT while the worker is joinable.
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(true, 8L << 20, nullptr, false));
    EXPECT_EQ(OB_SUCCESS, manager.register_work_area_profile(profile));
    EXPECT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(false, 4L << 20, nullptr, false));
  }
  worker.join();
  EXPECT_EQ(0, manager.get_active_profile_used()); // Do not hide a negative residual.
  EXPECT_EQ(0, manager.get_total_mem_used());
  EXPECT_EQ(0, profile.get_profile_total_used());
  ASSERT_EQ(OB_SUCCESS, manager.register_work_area_profile(profile));
  EXPECT_EQ(0, manager.get_total_mem_used());
  ASSERT_EQ(OB_SUCCESS, manager.unregister_work_area_profile(profile));
}

TEST(TestWorkareaMemoryPolicy, stores_spill_only_against_their_local_budget)
{
  ObSqlMemoryTracker tracker;
  DefaultPageAllocator allocator(ObMemAttr("WALocal", ObCtxIds::WORK_AREA));
  TestChunkDatumStore bounded("WABounded", allocator, tracker);
  DefaultBackedChunkDatumStore unlimited("WAUnlimited");
  ASSERT_EQ(OB_SUCCESS, bounded.init(1, ObCtxIds::WORK_AREA, "WABounded", true,
                                    0, ObChunkDatumStore::MIN_BLOCK_SIZE));
  ASSERT_EQ(OB_SUCCESS, unlimited.init(0, ObCtxIds::WORK_AREA, "WAUnlimited", true,
                                      0, ObChunkDatumStore::MIN_BLOCK_SIZE));
  bounded.set_callback(&tracker);
  const TestStoredRow row(3L << 10);
  for (int i = 0; i < 20; ++i) {
    ASSERT_EQ(OB_SUCCESS, bounded.add_row(row.get()));
    ASSERT_EQ(OB_SUCCESS, unlimited.add_row(row.get()));
  }
  EXPECT_GT(bounded.spilled_bytes(), 0);
  EXPECT_EQ(0, unlimited.spilled_bytes());
  EXPECT_EQ(20, bounded.get_row_cnt());
  EXPECT_EQ(20, unlimited.get_row_cnt());
}

TEST(TestWorkareaMemoryPolicy,
     dtl_default_store_remains_readable_after_handoff)
{
  TestSqlMemoryManagerGuard manager_guard;
  ASSERT_EQ(OB_SUCCESS, manager_guard.init());
  ObSqlMemoryManager &sql_manager = manager_guard.get();

  TestTmpFileManagerGuard tmp_file_manager_guard;
  ASSERT_EQ(OB_SUCCESS, tmp_file_manager_guard.init());
  oceanbase::sql::dtl::ObDTLIntermResultManager dtl_manager;
  const double saved_max_cpu = oceanbase::share::server_runtime()->max_cpu();
  oceanbase::share::server_runtime()->set_max_cpu(1);
  const int init_ret = dtl_manager.init();
  oceanbase::share::server_runtime()->set_max_cpu(saved_max_cpu);
  ASSERT_EQ(OB_SUCCESS, init_ret);
  oceanbase::sql::dtl::ObDTLIntermResultInfoGuard producer_guard;
  ObMemAttr attr("DtlQuotaTest", ObCtxIds::WORK_AREA);
  ASSERT_EQ(OB_SUCCESS,
            dtl_manager.create_interm_result_info(
                attr, producer_guard,
                oceanbase::sql::dtl::ObDTLIntermResultMonitorInfo()));
  ASSERT_NE(nullptr, producer_guard.result_info_);
  oceanbase::sql::dtl::ObDTLIntermResultInfo *result_info =
      producer_guard.result_info_;
  ASSERT_EQ(OB_SUCCESS,
            result_info->datum_store_->init(
                0, ObCtxIds::WORK_AREA, "DtlQuotaTest", false, 0,
                ObChunkDatumStore::MIN_BLOCK_SIZE));
  const TestStoredRow row(1L << 10);
  ASSERT_EQ(OB_SUCCESS, result_info->datum_store_->add_row(row.get()));
  result_info->set_eof(true);
  EXPECT_EQ(0, sql_manager.get_total_mem_used()); // No registered operator profile.

  oceanbase::sql::dtl::ObDTLIntermResultKey key;
  key.channel_id_ = 1;
  key.batch_id_ = 1;
  ASSERT_EQ(OB_SUCCESS,
            dtl_manager.insert_interm_result_info(key, result_info));
  producer_guard.reset();

  oceanbase::sql::dtl::ObDTLIntermResultInfoGuard consumer_guard;
  ASSERT_EQ(OB_SUCCESS,
            dtl_manager.atomic_get_interm_result_info(key, consumer_guard));
  ASSERT_NE(nullptr, consumer_guard.result_info_);
  ObChunkDatumStore::Iterator iterator;
  ASSERT_EQ(OB_SUCCESS,
            consumer_guard.result_info_->datum_store_->begin(iterator));
  const ObChunkDatumStore::StoredRow *stored_row = nullptr;
  ASSERT_EQ(OB_SUCCESS, iterator.get_next_row(stored_row));
  ASSERT_NE(nullptr, stored_row);
  EXPECT_EQ(row.get().row_size_, stored_row->row_size_);
  EXPECT_EQ(OB_ITER_END, iterator.get_next_row(stored_row));

  ASSERT_EQ(OB_SUCCESS, dtl_manager.erase_interm_result_info(key));
  consumer_guard.reset();
}


TEST(TestWorkareaMemoryPolicy, vectorized_batch_keeps_rows_pinned_and_propagates_oom)
{
  DefaultPageAllocator backing_allocator(ObMemAttr("WABatch", ObCtxIds::WORK_AREA));
  const int64_t batch_size = TEST_MAX_BATCH_SIZE;

  {
    DefaultBackedChunkDatumStore store("WABatchOK");
    ASSERT_EQ(OB_SUCCESS,
              store.init(0, ObCtxIds::WORK_AREA, "WABatchOK",
                         true, 0, ObChunkDatumStore::MIN_BLOCK_SIZE));
    std::vector<ObChunkDatumStore::StoredRow *> rows;
    ASSERT_EQ(OB_SUCCESS,
              add_string_vectorized_batch(store, backing_allocator,
                                          batch_size, rows));
    ASSERT_GT(store.get_block_cnt(), 1);
    for (int64_t i = 0; i < batch_size; ++i) {
      ASSERT_NE(nullptr, rows[i]);
      EXPECT_EQ(1, rows[i]->cnt_);
      const std::string expected = batch_value(i);
      EXPECT_EQ(ObString(expected.length(), expected.data()),
                rows[i]->cells()[0].get_string());
    }
    ObChunkDatumStore::Iterator iterator;
    ASSERT_EQ(OB_SUCCESS, store.begin(iterator));
    const ObChunkDatumStore::StoredRow *row = nullptr;
    for (int64_t i = 0; i < batch_size; ++i) {
      ASSERT_EQ(OB_SUCCESS, iterator.get_next_row(row));
      EXPECT_EQ(rows[i], row);
      EXPECT_EQ(1, row->cnt_);
      const std::string expected = batch_value(i);
      EXPECT_EQ(ObString(expected.length(), expected.data()),
                row->cells()[0].get_string());
    }
    EXPECT_EQ(OB_ITER_END, iterator.get_next_row(row));
  }

  FailingAllocator failing(backing_allocator);
  ObSqlMemoryTracker tracker;
  TestChunkDatumStore store("WABatchOOM", failing, tracker);
  ASSERT_EQ(OB_SUCCESS, store.init(1, ObCtxIds::WORK_AREA, "WABatchOOM", true,
                                  0, ObChunkDatumStore::MIN_BLOCK_SIZE));
  store.set_callback(&tracker);
  std::vector<ObChunkDatumStore::StoredRow *> seed_rows;
  ASSERT_EQ(OB_SUCCESS, add_string_vectorized_batch(store, backing_allocator, 1, seed_rows));
  ASSERT_EQ(1, seed_rows.size());
  auto *seed = seed_rows[0];
  failing.arm();
  std::vector<ObChunkDatumStore::StoredRow *> rows;
  EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED,
            add_string_vectorized_batch(store, backing_allocator, batch_size, rows));
  EXPECT_EQ(1, failing.failures());
  EXPECT_EQ(0, store.spilled_bytes());
  EXPECT_EQ(1, seed->cnt_);
  const std::string expected = batch_value(0);
  EXPECT_EQ(ObString(expected.length(), expected.data()), seed->cells()[0].get_string());
}

TEST(TestWorkareaMemoryPolicy, ra_datum_store_local_spill_and_readback)
{
  TestTmpFileManagerGuard tmp_file_manager;
  ASSERT_EQ(OB_SUCCESS, tmp_file_manager.init());
  std::vector<char> first_payload(40L << 10, 'a');
  std::vector<char> second_payload(40L << 10, 'b');
  ObDatum datum;
  datum.set_string(first_payload.data(), first_payload.size());
  ObSEArray<ObDatum, 1> row;
  ASSERT_EQ(OB_SUCCESS, row.push_back(datum));

  {
    TestRADatumStore store;
    ASSERT_EQ(OB_SUCCESS,
              store.init(ObRADatumStore::BIG_BLOCK_SIZE / 2,
                         ObCtxIds::WORK_AREA, "DefaultWARADatum"));
    ASSERT_EQ(OB_SUCCESS, store.add_row(row));
    // FULL_INPUT_SAMPLE uses this default-backed store with an operator budget.
    row.at(0).set_string(second_payload.data(), second_payload.size());
    for (int64_t i = 1; i < 32; ++i) {
      ASSERT_EQ(OB_SUCCESS, store.add_row(row));
    }
    EXPECT_GT(store.spilled_bytes(), 0);
    ASSERT_EQ(OB_SUCCESS, store.finish_add_row());

    ObRADatumStore::Reader reader(store);
    const ObRADatumStore::StoredRow *stored_row = nullptr;
    ASSERT_EQ(OB_SUCCESS, reader.get_row(0, stored_row));
    ASSERT_NE(nullptr, stored_row);
    EXPECT_EQ(ObString(first_payload.size(), first_payload.data()),
              stored_row->cells()[0].get_string());
    ASSERT_EQ(OB_SUCCESS, reader.get_row(31, stored_row));
    ASSERT_NE(nullptr, stored_row);
    EXPECT_EQ(ObString(second_payload.size(), second_payload.data()),
              stored_row->cells()[0].get_string());
  }

}



TEST(TestWorkareaMemoryPolicy, chunk_row_local_spill_and_readback)
{
  TestTmpFileManagerGuard tmp_file_manager;
  ASSERT_EQ(OB_SUCCESS, tmp_file_manager.init());
  ObSqlMemoryTracker tracker;
  DefaultPageAllocator backing_allocator(ObMemAttr("WAChunkRow", ObCtxIds::WORK_AREA));
  std::vector<char> first_payload(40L << 10, 'a');
  std::vector<char> second_payload(40L << 10, 'b');
  ObObj cell;
  cell.set_varchar(ObString(first_payload.size(), first_payload.data()));
  ObNewRow row;
  row.cells_ = &cell;
  row.count_ = 1;

  {
    TestChunkRowStore first_store(backing_allocator, tracker);
    TestChunkRowStore second_store(backing_allocator, tracker);
    ASSERT_EQ(OB_SUCCESS,
              first_store.init(1, ObCtxIds::WORK_AREA,
                               "WAChunkRowFirst", true));
    ASSERT_EQ(OB_SUCCESS,
              second_store.init(0, ObCtxIds::WORK_AREA,
                                "WAChunkRowSecond", true));
    ASSERT_EQ(OB_SUCCESS, first_store.alloc_dir_id());
    first_store.set_callback(&tracker);
    second_store.set_callback(&tracker);

    ASSERT_EQ(OB_SUCCESS, first_store.add_row(row));
    ASSERT_EQ(OB_SUCCESS, second_store.add_row(row));
    cell.set_varchar(ObString(second_payload.size(), second_payload.data()));
    ASSERT_EQ(OB_SUCCESS, first_store.add_row(row));
    EXPECT_EQ(2, first_store.get_row_cnt());
    EXPECT_EQ(1, second_store.get_row_cnt());
    EXPECT_GT(first_store.spilled_bytes(), 0);
    ASSERT_EQ(OB_SUCCESS, first_store.finish_add_row(false));

    ObChunkRowStore::Iterator iterator;
    ASSERT_EQ(OB_SUCCESS, first_store.begin(iterator));
    ObNewRow read_row;
    ObObj read_cell;
    read_row.cells_ = &read_cell;
    read_row.count_ = 1;
    ASSERT_EQ(OB_SUCCESS, iterator.get_next_row(read_row));
    EXPECT_EQ(ObString(first_payload.size(), first_payload.data()),
              read_row.cells_[0].get_string());
    ASSERT_EQ(OB_SUCCESS, iterator.get_next_row(read_row));
    EXPECT_EQ(ObString(second_payload.size(), second_payload.data()),
              read_row.cells_[0].get_string());
    EXPECT_EQ(OB_ITER_END, iterator.get_next_row(read_row));
  }

}
