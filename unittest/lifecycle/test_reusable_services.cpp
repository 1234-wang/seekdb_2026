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

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <gtest/gtest.h>
#include "lib/alloc/memory_dump.h"
#include "lib/oblog/ob_log.h"
#include "lib/thread/thread.h"
#include "lib/thread/ob_simple_thread_pool.h"
#include "lib/utility/ob_rate_limiter.h"

using namespace oceanbase::common;

namespace
{
class TemporaryLogFile
{
public:
  TemporaryLogFile()
  {
    const int fd = mkstemp(path_);
    if (fd >= 0) {
      close(fd);
      valid_ = true;
    }
  }
  ~TemporaryLogFile() { if (valid_) { unlink(path_); } }
  const char *path() const { return path_; }
  bool valid() const { return valid_; }
  std::string contents() const
  {
    std::ifstream input(path_);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
private:
  char path_[64] = "/tmp/seekdb-reusable-logger-XXXXXX";
  bool valid_ = false;
};

class TrackingLimiter : public oceanbase::lib::ObRateLimiter
{
public:
  explicit TrackingLimiter(const bool force_allows) : force_allows_(force_allows) {}
  bool is_force_allows() const override
  {
    ++force_checks_;
    return force_allows_;
  }
  void reset_force_allows() override {}
  int try_acquire(int64_t, int64_t, int64_t) override { return OB_SUCCESS; }
  int acquire(int64_t, int64_t, int64_t) override { return OB_SUCCESS; }
  mutable std::atomic<int> force_checks_{0};
private:
  bool force_allows_;
};

struct FormatterControl
{
  FormatterControl() : resume(release.get_future().share()) {}
  void allow_finish()
  {
    if (!released.exchange(true)) { release.set_value(); }
  }
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> resume;
  std::atomic<bool> released{false};
  std::atomic<bool> first{true};
};

class ControlledLogKV : public ObILogKV
{
public:
  ControlledLogKV(std::shared_ptr<FormatterControl> control, bool trace_retry)
      : control_(std::move(control)), trace_retry_(trace_retry) {}
  int print(char *buf, int64_t buf_len, int64_t &pos, bool) const override
  {
    const bool first = control_->first.exchange(false);
    if (first) {
      control_->entered.set_value();
      control_->resume.wait();
    }
    if (first && trace_retry_) { return OB_SIZE_OVERFLOW; }
    return logdata_printf(buf, buf_len, pos, "admitted-formatter-record");
  }
private:
  std::shared_ptr<FormatterControl> control_;
  bool trace_retry_;
};

class ReleaseFormatterAndJoin
{
public:
  ReleaseFormatterAndJoin(std::shared_ptr<FormatterControl> control, std::thread &thread)
      : control_(std::move(control)), thread_(thread) {}
  ~ReleaseFormatterAndJoin()
  {
    control_->allow_finish();
    if (thread_.joinable()) { thread_.join(); }
  }
private:
  std::shared_ptr<FormatterControl> control_;
  std::thread &thread_;
};

class TrackingRunWrapper : public oceanbase::lib::IRunWrapper
{
public:
  int pre_run() override { ++begun; return OB_SUCCESS; }
  int end_run() override { ++ended; return OB_SUCCESS; }
  uint64_t id() const override { return oceanbase::common::OB_SERVER_RUNTIME_ID; }
  std::atomic<int> begun{0};
  std::atomic<int> ended{0};
};

class RestoreDefaultWrapper
{
public:
  RestoreDefaultWrapper() : previous_(oceanbase::lib::Threads::get_default_run_wrapper()) {}
  ~RestoreDefaultWrapper() { oceanbase::lib::Threads::set_default_run_wrapper(previous_); }
private:
  oceanbase::lib::IRunWrapper *previous_;
};

class ImmediateThreadPool : public oceanbase::lib::ThreadPool
{
public:
  void run1() override {}
};

class CompletingSimplePool : public ObSimpleThreadPool
{
public:
  explicit CompletingSimplePool(std::promise<void> &completed) : completed_(completed) {}
  void handle(void *) override { completed_.set_value(); }
private:
  std::promise<void> &completed_;
};

class RepeatingSimplePool : public ObSimpleThreadPool
{
public:
  void handle(void *) override
  {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      ++completed_;
    }
    completed_cv_.notify_all();
  }

  bool wait_for_completed(const int64_t target)
  {
    std::unique_lock<std::mutex> guard(mutex_);
    return completed_cv_.wait_for(guard, std::chrono::seconds(5),
                                  [&] { return completed_ >= target; });
  }

private:
  std::mutex mutex_;
  std::condition_variable completed_cv_;
  int64_t completed_ = 0;
};

class ControlledLogWriter : public ObRingBufLogWriter
{
public:
  ControlledLogWriter() : release_(release_batch_.get_future()) {}
  ~ControlledLogWriter()
  {
    release_batch();
    stop();
    wait();
    destroy();
  }
  void wait_first_batch() { first_batch_.get_future().wait(); }
  void release_batch()
  {
    if (!released_) {
      release_batch_.set_value();
      released_ = true;
    }
  }
  int64_t processed() const { return processed_; }
protected:
  void process_batch(char **, int64_t *, int64_t count) override
  {
    if (0 == processed_) {
      first_batch_.set_value();
      release_.wait();
    }
    processed_ += count;
  }
private:
  std::promise<void> first_batch_;
  std::promise<void> release_batch_;
  std::future<void> release_;
  int64_t processed_ = 0;
  bool released_ = false;
};
}

TEST(ReusableServices, LogWriterDrainsCommittedRecordsOnStop)
{
  ControlledLogWriter writer;
  ASSERT_EQ(OB_SUCCESS, writer.init(1000, "ExitTest"));
  const int64_t first = writer.alloc(ObRingBufLogWriter::ENTRY_HEADER_SIZE + sizeof(uint64_t));
  ASSERT_GE(first, 0);
  writer.commit(first);
  writer.wait_first_batch();
  const int64_t second = writer.alloc(ObRingBufLogWriter::ENTRY_HEADER_SIZE + sizeof(uint64_t));
  ASSERT_GE(second, 0);
  writer.commit(second);
  writer.stop();
  writer.release_batch();
  writer.wait();
  EXPECT_EQ(2, writer.processed());
}

TEST(ReusableServices, LoggerFlushesAfterReinitialization)
{
  TemporaryLogFile file;
  ASSERT_TRUE(file.valid());
  ObLogger logger;
  logger.set_file_name(file.path(), true);
  logger.set_enable_async_log(true);
  const ObBaseLogWriterCfg cfg(1024, 1000, 1, 64);
  for (int cycle = 0; cycle < 3; ++cycle) {
    ASSERT_EQ(OB_SUCCESS, logger.init(cfg));
    EXPECT_TRUE(logger.is_async_log_used());
    EXPECT_EQ(OB_INIT_TWICE, logger.init(cfg));
    EXPECT_TRUE(logger.is_inited());
    const int log_fd = logger.get_svr_log().fd_;
    ASSERT_GT(log_fd, STDERR_FILENO);
    logger.log_message_fmt("[LIFECYCLE]", OB_LOG_LEVEL_INFO, __FILE__, __LINE__,
                           __func__, 0, "reusable-service-cycle-%d", cycle);
    // destroy() must join the flush worker and flush accepted records itself.
    logger.destroy();
    errno = 0;
    EXPECT_EQ(-1, fcntl(log_fd, F_GETFD));
    EXPECT_EQ(EBADF, errno);
    logger.destroy();
    EXPECT_FALSE(logger.is_inited());
    EXPECT_NE(std::string::npos, file.contents().find("reusable-service-cycle-" + std::to_string(cycle)));
  }
}

TEST(ReusableServices, LoggerPreservesConfiguredSyncMode)
{
  TemporaryLogFile file;
  ASSERT_TRUE(file.valid());
  ObLogger logger;
  logger.set_file_name(file.path(), true);
  logger.set_enable_async_log(false);
  const ObBaseLogWriterCfg cfg(1024, 1000, 1, 64);
  for (int cycle = 0; cycle < 2; ++cycle) {
    ASSERT_EQ(OB_SUCCESS, logger.init(cfg));
    EXPECT_FALSE(logger.is_async_log_used());
    logger.destroy();
  }
}

TEST(ReusableServices, ClosedLoggerDoesNotAllocateOrWriteToConsole)
{
  TemporaryLogFile file;
  ASSERT_TRUE(file.valid());
  ObLogger logger;
  logger.set_file_name(file.path(), true);
  const ObBaseLogWriterCfg cfg(1024, 1000, 1, 64);
  ASSERT_EQ(OB_SUCCESS, logger.init(cfg));
  logger.destroy();
  const int64_t allocations = logger.get_alloc_success_count();
  ::testing::internal::CaptureStderr();
  logger.log_message_fmt("[LIFECYCLE]", OB_LOG_LEVEL_INFO, __FILE__, __LINE__,
                         __func__, 0, "ordinary log after close");
  const std::string stderr_output = ::testing::internal::GetCapturedStderr();
  EXPECT_TRUE(stderr_output.empty());
  EXPECT_EQ(allocations, logger.get_alloc_success_count());
  EXPECT_EQ(std::string::npos, file.contents().find("ordinary log after close"));
}

TEST(ReusableServices, MemoryDumpCanReleaseAndReinitialize)
{
  ObMemoryDump dump;
  for (int cycle = 0; cycle < 3; ++cycle) {
    ASSERT_EQ(OB_SUCCESS, dump.init());
    EXPECT_TRUE(dump.is_inited());
    EXPECT_EQ(OB_INIT_TWICE, dump.init());
    EXPECT_TRUE(dump.is_inited());
    dump.destroy();
    dump.destroy();
    EXPECT_FALSE(dump.is_inited());
    EXPECT_EQ(OB_NOT_INIT, dump.generate_mod_stat_task());
  }
}

TEST(ReusableServices, HostThreadLimiterExpiresAcrossClose)
{
  ObLogger logger;
  logger.set_enable_log_limit(true);
  auto old_limiter = std::make_unique<TrackingLimiter>(true);
  TrackingLimiter new_limiter(false);
  ObLogger::set_default_limiter(*old_limiter);
  std::promise<void> bound;
  std::promise<void> reopened;
  auto resume = reopened.get_future();
  bool allowed_before_close = false;
  bool allowed_after_reopen = true;
  std::thread host_thread([&] {
    ObLogger::set_tl_limiter(*old_limiter);
    allowed_before_close = logger.is_force_allows();
    bound.set_value();
    resume.wait();
    allowed_after_reopen = logger.is_force_allows();
  });
  bound.get_future().wait();
  // The first call has completed; only its TLS pointer survives this close.
  ObLogger::clear_log_limiters();
  old_limiter.reset();
  ObLogger::set_default_limiter(new_limiter);
  reopened.set_value();
  host_thread.join();
  EXPECT_TRUE(allowed_before_close);
  EXPECT_FALSE(allowed_after_reopen);
  EXPECT_EQ(1, new_limiter.force_checks_.load());
  ObLogger::clear_log_limiters();
}

TEST(ReusableServices, LoggerCloseWaitsForAnAdmittedFormatter)
{
  using namespace std::chrono_literals;
  TemporaryLogFile file;
  ASSERT_TRUE(file.valid());
  ObLogger logger;
  logger.set_file_name(file.path(), true);
  const ObBaseLogWriterCfg cfg(1024, 1000, 1, 64);
  ASSERT_EQ(OB_SUCCESS, logger.init(cfg));
  auto control = std::make_shared<FormatterControl>();
  auto entered = control->entered.get_future();
  std::thread producer([&] {
    logger.log_message_kv("[LIFECYCLE]", OB_LOG_LEVEL_INFO, __FILE__, __LINE__,
                          __func__, 0, "controlled formatter", ControlledLogKV(control, false));
  });
  ReleaseFormatterAndJoin cleanup(control, producer);
  ASSERT_EQ(std::future_status::ready, entered.wait_for(5s));
  auto closing = std::async(std::launch::async, [&] { logger.destroy(); });
  EXPECT_EQ(std::future_status::timeout, closing.wait_for(50ms));
  control->allow_finish();
  ASSERT_EQ(std::future_status::ready, closing.wait_for(5s));
  closing.get();
  producer.join();
  EXPECT_NE(std::string::npos, file.contents().find("admitted-formatter-record"));
  EXPECT_FALSE(logger.is_inited());
  ASSERT_EQ(OB_SUCCESS, logger.init(cfg));
  logger.log_message_fmt("[LIFECYCLE]", OB_LOG_LEVEL_INFO, __FILE__, __LINE__,
                         __func__, 0, "record-after-formatter-close");
  logger.destroy();
  EXPECT_NE(std::string::npos, file.contents().find("record-after-formatter-close"));
}

TEST(ReusableServices, AdmittedTraceRetryDoesNotReenterClosedAdmission)
{
  using namespace std::chrono_literals;
  TemporaryLogFile file;
  ASSERT_TRUE(file.valid());
  ObLogger logger;
  logger.set_file_name(file.path(), true);
  ASSERT_EQ(OB_SUCCESS, logger.init(ObBaseLogWriterCfg(1024, 1000, 1, 64)));
  auto control = std::make_shared<FormatterControl>();
  auto entered = control->entered.get_future();
  std::thread producer([&] {
    logger.set_trace_mode(true);
    logger.log_message_fmt("[LIFECYCLE]", OB_LOG_LEVEL_INFO, __FILE__, __LINE__,
                           __func__, 0, "trace-before-close");
    // Return SIZE_OVERFLOW once while the trace buffer already contains a
    // record. The retry must flush using the outer admission, even during close.
    logger.log_message_kv("[LIFECYCLE]", OB_LOG_LEVEL_INFO, __FILE__, __LINE__,
                          __func__, 0, "trace retry", ControlledLogKV(control, true));
    logger.set_trace_mode(false);
  });
  ReleaseFormatterAndJoin cleanup(control, producer);
  ASSERT_EQ(std::future_status::ready, entered.wait_for(5s));
  auto closing = std::async(std::launch::async, [&] { logger.destroy(); });
  EXPECT_EQ(std::future_status::timeout, closing.wait_for(50ms));
  control->allow_finish();
  ASSERT_EQ(std::future_status::ready, closing.wait_for(5s));
  closing.get();
  producer.join();
  EXPECT_NE(std::string::npos, file.contents().find("trace-before-close"));
}

TEST(ReusableServices, ThreadPoolContextBelongsToItsOwner)
{
  TrackingRunWrapper first;
  TrackingRunWrapper second;
  RestoreDefaultWrapper restore;
  oceanbase::lib::Threads::set_default_run_wrapper(nullptr);
  ImmediateThreadPool process_pool;
  oceanbase::lib::Threads::set_default_run_wrapper(&first);
  ASSERT_EQ(OB_SUCCESS, process_pool.start());
  process_pool.wait();
  process_pool.destroy();
  EXPECT_EQ(0, first.begun.load());
  EXPECT_EQ(0, first.ended.load());

  ImmediateThreadPool database_pool;
  oceanbase::lib::Threads::set_default_run_wrapper(&second);
  ASSERT_EQ(OB_SUCCESS, database_pool.start());
  database_pool.wait();
  database_pool.destroy();
  EXPECT_EQ(1, first.begun.load());
  EXPECT_EQ(1, first.ended.load());
  EXPECT_EQ(0, second.begun.load());

  ImmediateThreadPool explicitly_bound;
  explicitly_bound.set_run_wrapper(&first);
  ASSERT_EQ(OB_SUCCESS, explicitly_bound.start());
  explicitly_bound.wait();
  explicitly_bound.destroy();
  EXPECT_EQ(2, first.begun.load());
  EXPECT_EQ(2, first.ended.load());
}

TEST(ReusableServices, SimplePoolReinitializationCreatesNewWorkers)
{
  auto &manager = ObSimpleThreadPoolDynamicMgr::get_instance();
  const int manager_ret = manager.init();
  ASSERT_TRUE(OB_SUCCESS == manager_ret || OB_INIT_TWICE == manager_ret);
  {
    RepeatingSimplePool pool;
    int task = 1;
    for (int cycle = 1; cycle <= 3; ++cycle) {
      ASSERT_EQ(OB_SUCCESS, pool.init(1, 8, "RepeatPool"));
      ASSERT_EQ(OB_SUCCESS, pool.push(&task));
      ASSERT_TRUE(pool.wait_for_completed(cycle)) << "cycle=" << cycle;
      pool.stop();
      pool.wait();
      pool.destroy();
      EXPECT_EQ(0, pool.worker_count()) << "cycle=" << cycle;
    }
  }
  if (OB_SUCCESS == manager_ret) {
    manager.destroy();
  }
}

TEST(ReusableServices, LazySimplePoolWorkersInheritTheOuterOwner)
{
  using namespace std::chrono_literals;
  TrackingRunWrapper database;
  RestoreDefaultWrapper restore;
  oceanbase::lib::Threads::set_default_run_wrapper(nullptr);
  auto &manager = ObSimpleThreadPoolDynamicMgr::get_instance();
  const int manager_ret = manager.init();
  ASSERT_TRUE(OB_SUCCESS == manager_ret || OB_INIT_TWICE == manager_ret);
  {
    std::promise<void> completed;
    auto done = completed.get_future();
    // Matches TimerService: its outer pool is constructed at process bring-up,
    // but child workers may be created only after a database opens.
    CompletingSimplePool process_pool(completed);
    oceanbase::lib::Threads::set_default_run_wrapper(&database);
    ASSERT_EQ(OB_SUCCESS, process_pool.init(1, 8, "OwnerTest"));
    int task = 1;
    ASSERT_EQ(OB_SUCCESS, process_pool.push(&task));
    ASSERT_EQ(std::future_status::ready, done.wait_for(5s));
    process_pool.destroy();
  }
  if (manager_ret == OB_SUCCESS) { manager.destroy(); }
  EXPECT_EQ(0, database.begun.load());
  EXPECT_EQ(0, database.ended.load());
}
