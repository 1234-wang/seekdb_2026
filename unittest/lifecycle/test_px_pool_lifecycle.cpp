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
#include <memory>
#include <thread>
#include <gtest/gtest.h>
#include "observer/omt/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::omt;

namespace
{
struct CallbackState
{
  CallbackState() : release_future(release.get_future().share()) {}
  void allow_finish()
  {
    if (!released.exchange(true)) {
      release.set_value();
    }
  }
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> release_future;
  std::atomic<bool> released{false};
  std::atomic<bool> executed{false};
  std::atomic<int> acquire_after_stop{OB_SUCCESS};
};

class ScopedPxPools
{
public:
  ScopedPxPools() : state(std::make_shared<CallbackState>()) {}
  ~ScopedPxPools()
  {
    // ASSERT failures must release a running callback before joining it.
    state->allow_finish();
    stop();
    wait();
  }
  void stop()
  {
    ObPxPools *ptr = &pools;
    ObPxPools::server_module_stop(ptr);
  }
  void wait()
  {
    ObPxPools *ptr = &pools;
    ObPxPools::server_module_wait(ptr);
  }
  std::shared_ptr<CallbackState> state;
  ObPxPools pools;
};
}

TEST(RuntimeLifecycle, PxWaitFinishesCallbacksAndRejectsLateWork)
{
  using namespace std::chrono_literals;
  for (int cycle = 0; cycle < 2; ++cycle) {
    SCOPED_TRACE(cycle);
    ScopedPxPools scoped;
    ASSERT_EQ(OB_SUCCESS, scoped.pools.init());
    ObPxPool *pool = nullptr;
    ASSERT_EQ(OB_SUCCESS, scoped.pools.get_or_create(1, pool));
    ASSERT_NE(nullptr, pool);
    auto entered = scoped.state->entered.get_future();
    const auto callback = [state = scoped.state, &scoped](bool execute) {
      state->executed = execute;
      state->entered.set_value();
      if (execute) {
        state->release_future.wait();
        ObPxPool *late_pool = nullptr;
        state->acquire_after_stop = scoped.pools.get_or_create(2, late_pool);
      }
    };
    // start() may return before the worker has incremented active_threads_.
    int ret = OB_SIZE_OVERFLOW;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
      ret = pool->submit(callback);
      if (ret == OB_SIZE_OVERFLOW) { std::this_thread::sleep_for(1ms); }
    } while (ret == OB_SIZE_OVERFLOW && std::chrono::steady_clock::now() < deadline);
    ASSERT_EQ(OB_SUCCESS, ret);
    ASSERT_EQ(std::future_status::ready, entered.wait_for(5s));
    ASSERT_TRUE(scoped.state->executed);

    scoped.stop();
    ObPxPool *late_pool = nullptr;
    EXPECT_EQ(OB_NOT_RUNNING, scoped.pools.get_or_create(2, late_pool));
    EXPECT_EQ(OB_NOT_RUNNING, pool->submit([](bool) {}));
    auto waiting = std::async(std::launch::async, [&scoped] { scoped.wait(); });
    // wait() must include the accepted callback, rather than only its producer.
    EXPECT_EQ(std::future_status::timeout, waiting.wait_for(50ms));
    scoped.state->allow_finish();
    ASSERT_EQ(std::future_status::ready, waiting.wait_for(5s));
    waiting.get();
    EXPECT_EQ(OB_NOT_RUNNING, scoped.state->acquire_after_stop.load());
    scoped.wait();
  }
}
