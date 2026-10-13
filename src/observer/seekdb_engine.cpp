/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "observer/include/seekdb_engine.h"
#include "observer/ob_command_line_parser.h"
#include "observer/ob_server.h"
#include "observer/ob_server_utils.h"
#include "share/ob_errno.h"
#include "lib/file/file_directory_utils.h"
#include "lib/thread/protected_stack_allocator.h"
#include "lib/worker.h"
#include <curl/curl.h>
#include <climits>
#include <condition_variable>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::observer;

struct SeekdbEngineImpl {
  std::mutex mutex;
  std::condition_variable condition;
  std::thread thread;
  std::vector<std::string> arguments;
  std::vector<const char *> argv;
  bool ready = false;
  bool closing = false;
  int result = OB_SUCCESS;
};

namespace {
std::mutex engine_mutex;
SeekdbEngineHandle active_engine = nullptr;
bool core_used = false;

// Unlike start_daemon(), retain the descriptor so close releases the PID lock.
int acquire_pid_file(int &fd)
{
  fd = ::open("run/seekdb.pid", O_RDWR | O_CREAT, 0600);
  if (fd < 0) { return OB_IO_ERROR; }
#ifdef _WIN32
  if (_locking(fd, _LK_NBLCK, 1) != 0) {
#else
  if (lockf(fd, F_TLOCK, 0) != 0) {
#endif
    ::close(fd);
    fd = -1;
    return OB_INIT_TWICE;
  }
#ifdef _WIN32
  if (_chsize(fd, 0) != 0) { return OB_IO_ERROR; }
#else
  if (ftruncate(fd, 0) != 0) { return OB_IO_ERROR; }
#endif
  char pid[32];
  const int length = snprintf(pid, sizeof(pid), "%d", static_cast<int>(getpid()));
  return ::write(fd, pid, length) == length ? OB_SUCCESS : OB_IO_ERROR;
}

void run_engine(SeekdbEngineHandle engine)
{
  lib::ObStackHeaderGuard stack_guard;
  lib::Worker worker;
  lib::Worker::set_worker_to_thread_local(&worker);
  int ret = OB_SUCCESS;
  int pid_fd = -1;
  bool entered_core = false;
  bool curl_initialized = false;
  bool changed_directory = false;
  char previous_directory[4096];
  ObServerOptions options;
  options.log_level_ = DEFAULT_LOG_LEVEL;
  options.in_process_ = true;
  options.nodaemon_ = true;
  const auto &argv = engine->argv;
  ObCommandLineParser parser;
  if (nullptr == getcwd(previous_directory, sizeof(previous_directory))) {
    ret = OB_IO_ERROR;
  } else if (OB_FAIL(parser.parse_args(static_cast<int>(argv.size()), argv.data(), options, false))) {
  } else if (OB_FAIL(ObServerUtils::prepare_directories(options))) {
  } else if (::chdir(options.base_dir_.ptr()) != 0) {
    ret = OB_IO_ERROR;
  } else {
    changed_directory = true;
#ifndef _WIN32
    struct stat info;
    if (::stat("etc", &info) == 0 && info.st_uid != getuid()) {
      ret = OB_FILE_OR_DIRECTORY_PERMISSION_DENIED;
    }
#endif
    for (const char *directory : {"run", "etc", "log"}) {
      if (OB_SUCC(ret)) { ret = FileDirectoryUtils::create_full_path(directory); }
    }
    if (OB_SUCC(ret)) { ret = acquire_pid_file(pid_fd); }
    if (OB_SUCC(ret)) {
      if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        curl_initialized = true;
        OB_LOGGER.set_log_level(options.log_level_);
        OB_LOGGER.set_file_name("log/seekdb.log", true /* no_redirect */, false);
        ObPLogWriterCfg log_config;
        core_used = true;
        entered_core = true;
        ObServer &server = ObServer::get_instance();
        if (OB_FAIL(server.init(options, log_config))) {
        } else if (OB_FAIL(server.start())) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    std::unique_lock<std::mutex> guard(engine->mutex);
    engine->ready = true;
    engine->condition.notify_all();
    engine->condition.wait(guard, [engine] { return engine->closing; });
  }
  if (entered_core) {
    ObServer &server = ObServer::get_instance();
    server.set_stop();
    const int stop_ret = server.wait();
    server.destroy();
    if (OB_SUCC(ret)) { ret = stop_ret; }
  }
  if (curl_initialized) { curl_global_cleanup(); }
  if (pid_fd >= 0) {
    // Remove our pathname while the lock is still held.
    if (::unlink("run/seekdb.pid") != 0 && OB_SUCC(ret)) { ret = OB_IO_ERROR; }
    if (::close(pid_fd) != 0 && OB_SUCC(ret)) { ret = OB_IO_ERROR; }
  }
  if (changed_directory && ::chdir(previous_directory) != 0 && OB_SUCC(ret)) {
    ret = OB_IO_ERROR;
  }
  lib::Worker::set_worker_to_thread_local(nullptr);
  std::lock_guard<std::mutex> guard(engine->mutex);
  engine->result = ret;
  engine->ready = true;
  engine->condition.notify_all();
}
} // namespace

SeekdbEngineHandle seekdb_engine_open(int argc, const char *const argv[], int *error_code,
                                     const char **error_message)
{
  std::lock_guard<std::mutex> lifecycle_guard(engine_mutex);
  int ret = OB_SUCCESS;
  SeekdbEngineHandle engine = nullptr;
  if (argc < 0 || argc == INT_MAX || (argc > 0 && argv == nullptr)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (active_engine != nullptr) {
    ret = OB_INIT_TWICE;
  } else if (core_used) {
    // Do not pretend the current singleton graph can be initialized twice.
    ret = OB_NOT_SUPPORTED;
  } else {
    for (int i = 0; i < argc; ++i) {
      if (argv[i] == nullptr) { ret = OB_INVALID_ARGUMENT; break; }
    }
    if (OB_SUCC(ret)) {
      try {
        engine = new SeekdbEngineImpl;
        // The internal command-line parser expects a program name at argv[0].
        engine->arguments.emplace_back("seekdb");
        for (int i = 0; i < argc; ++i) { engine->arguments.emplace_back(argv[i]); }
        for (const auto &argument : engine->arguments) { engine->argv.push_back(argument.c_str()); }
        engine->thread = std::thread(run_engine, engine);
        std::unique_lock<std::mutex> guard(engine->mutex);
        engine->condition.wait(guard, [engine] { return engine->ready; });
        ret = engine->result;
      } catch (const std::bad_alloc &) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } catch (const std::system_error &) {
        ret = OB_ERR_UNEXPECTED;
      }
      if (OB_FAIL(ret)) {
        if (engine != nullptr && engine->thread.joinable()) {
          {
            std::lock_guard<std::mutex> guard(engine->mutex);
            engine->closing = true;
            engine->condition.notify_all();
          }
          engine->thread.join();
        }
        delete engine;
        engine = nullptr;
      } else {
        active_engine = engine;
      }
    }
  }
  if (error_code != nullptr) { *error_code = ret; }
  if (error_message != nullptr) { *error_message = OB_SUCCESS == ret ? nullptr : ob_strerror(ret); }
  return engine;
}

int seekdb_engine_close(SeekdbEngineHandle engine)
{
  std::lock_guard<std::mutex> lifecycle_guard(engine_mutex);
  if (engine == nullptr) { return OB_SUCCESS; }
  if (engine != active_engine) { return OB_INVALID_ARGUMENT; }
  {
    std::lock_guard<std::mutex> guard(engine->mutex);
    engine->closing = true;
    engine->condition.notify_all();
  }
  engine->thread.join();
  const int ret = engine->result;
  active_engine = nullptr;
  delete engine;
  return ret;
}
