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
#include <thread>
#include <gtest/gtest.h>
#include "share/ob_debug_sync.h"

using namespace oceanbase::common;

TEST(DebugSyncLifecycle, CancelledWaiterDrainsBeforeRestart)
{
  using namespace std::chrono_literals;
  ObDSEventControl events;
  const ObSyncEventName event_name("restart-waiter");
  for (int cycle = 0; cycle < 2; ++cycle) {
    SCOPED_TRACE(cycle);
    ASSERT_EQ(OB_SUCCESS, events.start());
    auto waiting = std::async(std::launch::async, [&] {
      return events.wait(event_name, 10 * 1000 * 1000, true);
    });
    // start() refuses to reset an event with a registered waiter. Polling this
    // condition avoids racing stop() with the worker's entry into wait().
    int restart_ret = OB_SUCCESS;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
      restart_ret = events.start();
      if (OB_SUCCESS == restart_ret) { std::this_thread::sleep_for(1ms); }
    } while (OB_SUCCESS == restart_ret && std::chrono::steady_clock::now() < deadline);
    EXPECT_EQ(OB_EAGAIN, restart_ret);
    events.stop();
    ASSERT_EQ(std::future_status::ready, waiting.wait_for(5s));
    EXPECT_EQ(OB_CANCELED, waiting.get());
    // The cancelled waiter must have decremented its reference before return.
    ASSERT_EQ(OB_SUCCESS, events.start());
    ASSERT_EQ(OB_SUCCESS, events.signal(event_name));
    EXPECT_EQ(OB_SUCCESS, events.wait(event_name, 1000 * 1000, true));
  }
}

TEST(DebugSyncLifecycle, SurvivingHostThreadDropsOldActionsOnRestart)
{
  ASSERT_EQ(OB_SUCCESS, GDS.start());
  std::promise<void> bound;
  std::promise<void> resume;
  auto resumed = resume.get_future().share();
  int add_ret = OB_ERR_UNEXPECTED;
  bool same_storage = false;
  bool old_actions_cleared = false;
  std::thread host([&] {
    ObDSActionArray *old_actions = GDS.thread_local_actions();
    if (nullptr != old_actions) {
      ObDebugSyncAction action;
      action.sync_point_ = NOW;
      action.execute_ = 1;
      action.signal_.assign("old-lifetime-action");
      add_ret = old_actions->add_action(action);
    }
    bound.set_value();
    resumed.wait();
    ObDSActionArray *new_actions = GDS.thread_local_actions();
    same_storage = old_actions == new_actions;
    old_actions_cleared = nullptr != new_actions && new_actions->is_empty();
  });
  // No fatal assertion between starting the host and releasing/joining it.
  bound.get_future().wait();
  EXPECT_EQ(OB_SUCCESS, add_ret);
  GDS.stop();
  EXPECT_EQ(OB_SUCCESS, GDS.start());
  resume.set_value();
  host.join();
  EXPECT_TRUE(same_storage);
  EXPECT_TRUE(old_actions_cleared);
  GDS.stop();
}
