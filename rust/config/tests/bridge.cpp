// Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0
#include "config_bridge.h"
#include "local_bridge.h"
#include <cassert>
#include <string>

namespace config = oceanbase::config;
namespace local = oceanbase::local;

int main(int argc, char **argv)
{
  assert(argc == 2);
  assert(!config::load_active(true).ok);
  assert(local::open_storage(argv[1]) == 0);
  assert(config::load_active(true).ok);
  assert(config::cpu_count() == 0);
  int checks = 0;
  const config::CheckCallback accept([&] {
    ++checks;
    assert(config::cpu_count() == 0);
    return 0;
  });
  assert(config::update_checked("cpu_count", "4", false, accept).ok);
  assert(checks == 1 && config::cpu_count() == 4);
  auto rejected = config::update_checked("cpu_count", "8", false,
      config::CheckCallback([] { return -4002; }));
  assert(!rejected.ok && !rejected.commit_uncertain);
  assert(std::string(rejected.message).find("-4002") != std::string::npos);
  assert(config::cpu_count() == 4);
  assert(!config::update_checked("cpu_count", "-1", false, accept).ok);
  assert(checks == 1);
  const config::CheckCallback noop([] { return 0; });
  assert(!config::update_checked("unknown", "1", false, noop).ok);
  assert(config::update_checked("cpu_count", "8", false, config::CheckCallback([&] {
    auto nested = config::update_checked("cpu_count", "16", false, noop);
    assert(!nested.ok);
    assert(std::string(nested.message).find("reentered") != std::string::npos);
    return -4002;
  })).ok == false);
  assert(config::cpu_count() == 4);
  assert(!config::bootstrap_set("data_dir", std::string(1, '\xff')).ok);
  assert(!config::update_checked("data_dir", std::string(1, '\xff'), false, noop).ok);
  assert(!config::update_internal_state(std::string(1, '\xff'), "1").ok);
  assert(!config::update_internal_state("cpu_count", "1").ok);
  assert(config::update_internal_state("server_create_time", "123").ok);
  assert(config::server_create_time() == 123);
  assert(config::update_checked("net_thread_count", "5", false, noop).ok);
  assert(config::net_thread_count() == 0);
  assert(config::load_active(false).ok);
  assert(config::net_thread_count() == 0);
  assert(local::close_storage() == 0);
  assert(local::open_storage(argv[1]) == 0);
  assert(config::load_active(true).ok);
  assert(config::cpu_count() == 4 && config::net_thread_count() == 5);
  assert(config::server_create_time() == 123);
  // Reset ignores the supplied value and persists removal of the override.
  assert(config::update_checked("cpu_count", std::string(1, '\xff'), true, noop).ok);
  assert(config::cpu_count() == 0);
  assert(config::bootstrap_set("memory_budget", "2G").ok);
  assert(config::bootstrap_set("net_thread_count", "3").ok);
  assert(config::memory_budget() == 2LL * 1024 * 1024 * 1024);
  assert(config::net_thread_count() == 3);
  assert(config::save_bootstrap().ok);
  assert(local::close_storage() == 0);
  assert(local::open_storage(argv[1]) == 0);
  assert(config::load_active(true).ok);
  assert(config::cpu_count() == 0 && config::net_thread_count() == 3);
  assert(config::memory_budget() == 2LL * 1024 * 1024 * 1024);
  assert(local::close_storage() == 0);
}
