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
#include <set>
#include <thread>
#include <gtest/gtest.h>
#include "rootserver/ob_thread_idling.h"

using namespace oceanbase::common;

namespace {
class ThirtySecondIdler final : public oceanbase::rootserver::ObThreadIdling
{
public:
  explicit ThirtySecondIdler(volatile bool &stop) : ObThreadIdling(stop) {}
  int64_t get_idle_interval_us() override
  {
    // idle invokes this callback under its condition mutex, immediately before
    // waiting. Count distinct callers so a polling waiter cannot count twice.
    if (entered_threads_.insert(std::this_thread::get_id()).second) {
      entered_.fetch_add(1, std::memory_order_release);
    }
    return 30 * 1000 * 1000;
  }
  std::atomic<int> entered_{0};
private:
  std::set<std::thread::id> entered_threads_;
};
}

TEST(ThreadIdlingLifecycle, StopRemainsVisibleToEveryLateWaiter)
{
  using namespace std::chrono_literals;
  volatile bool stopped = false;
  ThirtySecondIdler idler(stopped);
  ATOMIC_STORE(&stopped, true);
  idler.wakeup();
  auto first = std::async(std::launch::async, [&] { return idler.idle(); });
  auto second = std::async(std::launch::async, [&] { return idler.idle(); });
  EXPECT_EQ(std::future_status::ready, first.wait_for(2s));
  EXPECT_EQ(std::future_status::ready, second.wait_for(2s));
  // Complete cleanup even when an expectation fails.
  idler.wakeup();
  EXPECT_EQ(OB_CANCELED, first.get());
  EXPECT_EQ(OB_CANCELED, second.get());
}

TEST(ThreadIdlingLifecycle, BroadcastCancelsMultipleWaitingCallers)
{
  using namespace std::chrono_literals;
  volatile bool stopped = false;
  ThirtySecondIdler idler(stopped);
  auto first = std::async(std::launch::async, [&] { return idler.idle(); });
  auto second = std::async(std::launch::async, [&] { return idler.idle(); });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (idler.entered_.load(std::memory_order_acquire) < 2
         && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(2, idler.entered_.load(std::memory_order_acquire));
  ATOMIC_STORE(&stopped, true);
  idler.wakeup();
  EXPECT_EQ(std::future_status::ready, first.wait_for(2s));
  EXPECT_EQ(std::future_status::ready, second.wait_for(2s));
  idler.wakeup();
  EXPECT_EQ(OB_CANCELED, first.get());
  EXPECT_EQ(OB_CANCELED, second.get());
}
