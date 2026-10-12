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
#include <chrono>
#include <future>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <map>
#include <optional>
#include <dirent.h>
#include <cstdio>
#include <deque>
#include <fstream>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <gtest/gtest.h>
#include "observer/ob_server.h"
#include "storage/ddl/ob_ddl_redo_log_writer.h"
#include "lib/worker.h"
#include "lib/thread/protected_stack_allocator.h"
#include "lib/allocator/ob_jemalloc.h"
#include "observer/ob_server_options.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/optimizer/stat/ob_opt_stat_manager.h"
#include "common/mysqlclient/ob_mysql_result.h"

namespace oceanbase {
namespace observer {
namespace {

struct ProcessThreadSnapshot
{
  bool valid = true;
  size_t jemalloc_threads = 0;
  std::map<long, std::string> other_threads;
};

ProcessThreadSnapshot process_threads()
{
  ProcessThreadSnapshot snapshot;
  DIR *directory = opendir("/proc/self/task");
  if (directory == nullptr) {
    snapshot.valid = false;
    return snapshot;
  }
  while (dirent *entry = readdir(directory)) {
    if (entry->d_name[0] == '.') { continue; }
    const long tid = std::strtol(entry->d_name, nullptr, 10);
    const std::string path = std::string("/proc/self/task/") + entry->d_name;
    std::ifstream comm(path + "/comm");
    std::string name;
    if (!std::getline(comm, name)) {
      // A joined thread can disappear while /proc is being enumerated.
      if (access(path.c_str(), F_OK) == 0 || errno != ENOENT) {
        snapshot.valid = false;
      }
    } else if (name == "jemalloc_bg_thd") {
      // These process allocator workers can be created lazily for new arenas.
      ++snapshot.jemalloc_threads;
    } else {
      snapshot.other_threads.emplace(tid, name);
    }
  }
  closedir(directory);
  return snapshot;
}

bool jemalloc_background_thread_limit(size_t &limit)
{
#if defined(OB_HAVE_BUNDLED_JEMALLOC) && !defined(ENABLE_SANITY)
  size_t size = sizeof(limit);
  return 0 == je_mallctl("max_background_threads", &limit, &size, nullptr, 0);
#else
  limit = 0;
  return true;
#endif
}

struct ProcessFdSnapshot
{
  bool valid = true;
  size_t anonymous_fds = 0;
  std::map<long, std::string> targets;
};

ProcessFdSnapshot process_fds()
{
  ProcessFdSnapshot snapshot;
  DIR *directory = opendir("/proc/self/fd");
  if (directory == nullptr) {
    snapshot.valid = false;
    return snapshot;
  }
  const int scanner_fd = dirfd(directory);
  while (dirent *entry = readdir(directory)) {
    if (entry->d_name[0] == '.') { continue; }
    const long fd = std::strtol(entry->d_name, nullptr, 10);
    if (fd == scanner_fd) { continue; }
    const std::string path = std::string("/proc/self/fd/") + entry->d_name;
    char target[PATH_MAX + 1];
    const ssize_t length = readlink(path.c_str(), target, PATH_MAX);
    if (length < 0) {
      if (errno != ENOENT) { snapshot.valid = false; }
      continue;
    }
    target[length] = '\0';
    const std::string name(target);
    snapshot.targets.emplace(fd, name);
    if (name.rfind("anon_inode:", 0) == 0
        || name.rfind("pipe:[", 0) == 0
        || name.rfind("socket:[", 0) == 0) {
      ++snapshot.anonymous_fds;
    }
  }
  closedir(directory);
  return snapshot;
}

void print_failure_log_tail(const std::string &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) { return; }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  constexpr std::streamoff tail_bytes = 64 * 1024;
  if (size > tail_bytes) {
    input.seekg(size - tail_bytes);
    std::string partial;
    std::getline(input, partial);
  } else {
    input.seekg(0);
  }
  std::deque<std::string> failures;
  for (std::string line; std::getline(input, line); ) {
    if (line.find("ERROR") != std::string::npos
        || line.find("WARN") != std::string::npos || line.find("WDIAG") != std::string::npos || line.find("EDIAG") != std::string::npos
        || line.find("fail") != std::string::npos) {
      failures.push_back(line.substr(0, 1536));
      if (failures.size() > 20) { failures.pop_front(); }
    }
  }
  if (!failures.empty()) {
    std::printf("Lifecycle failure diagnostics from %s:\n", path.c_str());
    for (const auto &line : failures) { std::printf("%s\n", line.c_str()); }
  }
}

// Exercise the existing engine phases. Public open/close belongs to bindings.
int start_server(ObServer &server, const ObServerOptions &options,
                 const ObPLogWriterCfg &log_config)
{
  int ret = server.init(options, log_config);
  if (ret == common::OB_SUCCESS) { ret = server.start(); }
  return ret;
}

int destroy_server(ObServer &server)
{
  int ret = server.stop();
  if (ret == common::OB_SUCCESS) { ret = server.wait(); }
  if (ret == common::OB_SUCCESS) { ret = server.destroy(); }
  return ret;
}

class BlockingDdlWriteChecker : public storage::ObDDLNeedStopWriteChecker
{
public:
  bool check_need_stop_write() override
  {
    if (!entered.exchange(true)) { waiting.set_value(); }
    return true;
  }
  std::atomic<bool> entered{false};
  std::promise<void> waiting;
};

void expect_ddl_wait_canceled_by_server_stop(ObServer &server)
{
  using namespace std::chrono_literals;
  BlockingDdlWriteChecker checker;
  auto waiting = checker.waiting.get_future();
  auto result = std::async(std::launch::async, [&checker] {
    lib::ObStackHeaderGuard stack_header_guard;
    lib::Worker worker;
    lib::Worker::set_worker_to_thread_local(&worker);
    int64_t sleep_us = 0;
    const int ret = storage::ObDDLCtrlSpeedHandle::get_instance().limit_and_sleep(
        0, 1, checker, sleep_us);
    lib::Worker::set_worker_to_thread_local(nullptr);
    return ret;
  });
  const bool entered = waiting.wait_for(5s) == std::future_status::ready;
  EXPECT_TRUE(entered) << "DDL limiter did not enter its real stop-write wait";
  if (entered) {
    EXPECT_TRUE(checker.entered.load());
    EXPECT_EQ(std::future_status::timeout, result.wait_for(50ms));
  }
  // Even an entry timeout must stop before the future joins its host thread.
  EXPECT_EQ(common::OB_SUCCESS, server.stop());
  const auto status = result.wait_for(5s);
  EXPECT_EQ(std::future_status::ready, status);
  if (status != std::future_status::ready) {
    // Keep the failure bounded by CTest if the engine forgot its stop hook.
    storage::ObDDLCtrlSpeedHandle::get_instance().stop();
  }
  EXPECT_EQ(common::OB_IN_STOP_STATE, result.get());
}

class ServerLifecycleTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    // Take the host baseline before the first fixture opens any database.
    // Reuse it so a leak from the failed-init/retry case cannot become a host
    // thread accepted by the subsequent three-cycle case.
    host_threads_ = &host_thread_baseline();
    ASSERT_TRUE(host_threads_->valid);
    ASSERT_FALSE(host_threads_->other_threads.empty());
    char directory[] = "/tmp/seekdb-lifecycle-XXXXXX";
    char *created = mkdtemp(directory);
    ASSERT_NE(nullptr, created);
    base_dir_ = created;
    old_dir_ = std::filesystem::current_path();
    std::filesystem::create_directories(base_dir_ + "/run");
    char log_directory[] = "/tmp/seekdb-lifecycle-host-log-XXXXXX";
    char *created_log_directory = mkdtemp(log_directory);
    ASSERT_NE(nullptr, created_log_directory);
    host_log_dir_ = created_log_directory;
    std::filesystem::create_directories(base_dir_ + "/etc");
    std::filesystem::current_path(base_dir_);
    OB_LOGGER.set_log_level("WDIAG");
    const std::string log_path = host_log_dir_ + "/lifecycle.log";
    OB_LOGGER.set_file_name(log_path.c_str(), true /*no_redirect_flag*/);
  }

  void TearDown() override
  {
    // Core shutdown phases must be idempotent after failed initialization.
    EXPECT_EQ(common::OB_SUCCESS, destroy_server(ObServer::get_instance()));
    if (!old_dir_.empty()) { std::filesystem::current_path(old_dir_); }
    if (!base_dir_.empty()) {
      if (HasFailure()) {
        std::printf("Lifecycle failure directory retained: %s\n", base_dir_.c_str());
        print_failure_log_tail(host_log_dir_ + "/lifecycle.log");
        print_failure_log_tail(host_log_dir_ + "/lifecycle.log.wf");
        std::fflush(stdout);
      } else {
        std::filesystem::remove_all(base_dir_);
      }
    }
    if (!host_log_dir_.empty()) {
      if (HasFailure()) {
        std::printf("Lifecycle host log directory retained: %s\n", host_log_dir_.c_str());
      } else {
        std::filesystem::remove_all(host_log_dir_);
      }
    }
  }

  int configure(ObServerOptions &options, bool invalid = false)
  {
    int ret = options.base_dir_.assign(base_dir_.c_str());
    const char *settings[][2] = {
      {"memory_budget", invalid ? "invalid" : "1G"},
      {"memory_chunk_cache_size", "16M"},
      {"syslog_level", "WDIAG"},
      {"log_disk_size", "2G"},
      {"datafile_size", "128M"},
      {"cpu_count", "2"},
      {"server_cpu_quota_min", "1"},
      {"server_cpu_quota_max", "1"},
      {"net_thread_count", "1"},
      {"mysql_port_mode", "disabled"},
      {"enable_rpc_service", "false"},
    };
    options.nodaemon_ = true;
    for (const auto &setting : settings) {
      if (ret == common::OB_SUCCESS) {
        ret = options.parameters_.push_back({
          common::ObString::make_string(setting[0]),
          common::ObString::make_string(setting[1])});
      }
    }
    return ret;
  }

  int write(const char *sql)
  {
    int64_t affected = 0;
    return ObServer::get_instance().get_mysql_proxy().write(sql, affected);
  }

  int row_count(int64_t &count)
  {
    common::ObMySQLProxy::MySQLResult result;
    int ret = ObServer::get_instance().get_mysql_proxy().read(
        result, "SELECT COUNT(*) AS cnt FROM lifecycle_test.records");
    if (ret == common::OB_SUCCESS) {
      common::sqlclient::ObMySQLResult *rows = result.get_result();
      ret = rows == nullptr ? common::OB_ERR_UNEXPECTED : rows->next();
      if (ret == common::OB_SUCCESS) { ret = rows->get_int("cnt", count); }
    }
    return ret;
  }

  static const ProcessThreadSnapshot &host_thread_baseline()
  {
    static const ProcessThreadSnapshot baseline = process_threads();
    return baseline;
  }

  static std::optional<size_t> &closed_anonymous_fd_baseline()
  {
    // Process support libraries can lazily initialize during the first full
    // engine start. Only that first successful start/shutdown sets this value.
    static std::optional<size_t> baseline;
    return baseline;
  }

  void expect_closed_resources(bool successful_start)
  {
    // These exact EDIAG signatures identify reference releases crossing a
    // database lifetime; expected invalid configuration/EAGAIN logs are allowed.
    std::ifstream log(host_log_dir_ + "/lifecycle.log");
    ASSERT_TRUE(log.good());
    for (std::string line; std::getline(log, line); ) {
      const bool wrong_macro_release = line.find("failed to dec macro block ref cnt")
          != std::string::npos;
      EXPECT_FALSE(wrong_macro_release) << line;
    }
    const ProcessThreadSnapshot threads = process_threads();
    ASSERT_TRUE(threads.valid);
    ASSERT_FALSE(threads.other_threads.empty());
    ASSERT_NE(nullptr, host_threads_);
    for (const auto &[tid, name] : threads.other_threads) {
      EXPECT_EQ(1U, host_threads_->other_threads.count(tid))
          << "Database thread survived shutdown: tid=" << tid << ", name=" << name;
    }
    if (threads.jemalloc_threads != 0) {
      size_t maximum = 0;
      ASSERT_TRUE(jemalloc_background_thread_limit(maximum));
      EXPECT_LE(threads.jemalloc_threads, maximum);
    }
    const ProcessFdSnapshot fds = process_fds();
    ASSERT_TRUE(fds.valid);
    ASSERT_FALSE(fds.targets.empty());
    for (const auto &[fd, target] : fds.targets) {
      EXPECT_NE(0U, target.rfind(base_dir_ + "/", 0))
          << "Database file survived shutdown: fd=" << fd << ", target=" << target;
    }
    std::optional<size_t> &baseline = closed_anonymous_fd_baseline();
    if (baseline.has_value()) {
      EXPECT_LE(fds.anonymous_fds, baseline.value())
          << "Anonymous file descriptors grew after reinitialization";
    } else if (successful_start) {
      baseline = fds.anonymous_fds;
    }
  }

  const ProcessThreadSnapshot *host_threads_ = nullptr;
  std::string base_dir_;
  std::string host_log_dir_;
  std::filesystem::path old_dir_;
};

TEST_F(ServerLifecycleTest, failed_initialization_can_be_destroyed_and_retried)
{
  ObServerOptions invalid;
  ASSERT_EQ(common::OB_SUCCESS, configure(invalid, true));
  ObPLogWriterCfg log_config;
  ObServer &server = ObServer::get_instance();
  EXPECT_NE(common::OB_SUCCESS, start_server(server, invalid, log_config));
  ASSERT_EQ(common::OB_SUCCESS, destroy_server(server));
  expect_closed_resources(false);
  ObServerOptions valid;
  ASSERT_EQ(common::OB_SUCCESS, configure(valid));
  ASSERT_EQ(common::OB_SUCCESS, start_server(server, valid, log_config));
  ASSERT_FALSE(server.is_stopped());
  ASSERT_EQ(common::OB_SUCCESS, destroy_server(server));
  expect_closed_resources(true);
}

TEST_F(ServerLifecycleTest, repeated_initialization_shutdown_preserves_data_and_releases_resources)
{
  const pid_t host_pid = getpid();
  int64_t last_generated_id = 0;
  ObServer &server = ObServer::get_instance();
  ObPLogWriterCfg log_config;
  for (int cycle = 0; cycle != 3; ++cycle) {
    SCOPED_TRACE(cycle);
    ObServerOptions options;
    ASSERT_EQ(common::OB_SUCCESS, configure(options));
    ASSERT_EQ(common::OB_SUCCESS, start_server(server, options, log_config));
    ASSERT_EQ(host_pid, getpid());
    ASSERT_FALSE(server.is_stopped());
    ASSERT_TRUE(share::g_server_modules_ready);
    ASSERT_TRUE(common::ObOptStatManager::get_instance().is_inited());
    ASSERT_EQ(common::OB_SUCCESS, write("CREATE DATABASE IF NOT EXISTS lifecycle_test"));
    ASSERT_EQ(common::OB_SUCCESS, write(
        "CREATE TABLE IF NOT EXISTS lifecycle_test.records (id INT PRIMARY KEY)"));
    int64_t count = -1;
    ASSERT_EQ(common::OB_SUCCESS, row_count(count));
    ASSERT_EQ(cycle, count);
    std::string insert = "INSERT INTO lifecycle_test.records VALUES (" + std::to_string(cycle) + ")";
    ASSERT_EQ(common::OB_SUCCESS, write(insert.c_str()));
    ASSERT_EQ(common::OB_SUCCESS, write(
        "CREATE TABLE IF NOT EXISTS lifecycle_test.generated_records "
        "(id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY)"));
    ASSERT_EQ(common::OB_SUCCESS, write(
        "INSERT INTO lifecycle_test.generated_records () VALUES ()"));
    {
      common::ObMySQLProxy::MySQLResult result;
      ASSERT_EQ(common::OB_SUCCESS, server.get_mysql_proxy().read(result,
          "SELECT COUNT(*) AS cnt, MAX(id) AS highest FROM lifecycle_test.generated_records"));
      common::sqlclient::ObMySQLResult *rows = result.get_result();
      ASSERT_NE(nullptr, rows);
      ASSERT_EQ(common::OB_SUCCESS, rows->next());
      int64_t generated_count = 0;
      int64_t highest = 0;
      ASSERT_EQ(common::OB_SUCCESS, rows->get_int("cnt", generated_count));
      ASSERT_EQ(common::OB_SUCCESS, rows->get_int("highest", highest));
      EXPECT_EQ(cycle + 1, generated_count);
      EXPECT_GT(highest, last_generated_id);
      last_generated_id = highest;
    } // Release SQL results/schema handles before shutdown.
    expect_ddl_wait_canceled_by_server_stop(server);
    ASSERT_EQ(common::OB_SUCCESS, destroy_server(server));
    ASSERT_EQ(common::OB_SUCCESS, destroy_server(server));
    ASSERT_TRUE(server.is_stopped());
    ASSERT_FALSE(share::g_server_modules_ready);
    ASSERT_FALSE(common::ObOptStatManager::get_instance().is_inited());
    ASSERT_FALSE(common::ObOptStatManager::get_instance().get_stat_sql_service().is_inited());
    ASSERT_EQ(&share::g_bootstrap_server_runtime, share::g_server_runtime);
    expect_closed_resources(true);
  }
}

} // namespace
} // namespace observer
} // namespace oceanbase
