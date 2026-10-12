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

#include <future>
#include <thread>
#include <gtest/gtest.h>
#include "lib/ob_running_mode.h"
#include "share/interrupt/ob_global_interrupt_call.h"

using namespace oceanbase::common;

TEST(InterruptRegistryLifecycle, InitialCapacityIsFixedForTheProcess)
{
  auto *mgr = ObGlobalInterruptManager::getInstance();
  ASSERT_NE(nullptr, mgr);
  ASSERT_EQ(OB_SUCCESS, mgr->ensure_initialized());
  const int64_t capacity = mgr->get_map().bucket_count();
  ASSERT_GT(capacity, 0);
  EXPECT_EQ(OB_INIT_TWICE, mgr->init());
  auto &mode = oceanbase::lib::ObRunningModeConfig::instance();
  const bool original_mini_mode = mode.mini_mode_;
  mode.mini_mode_ = !original_mini_mode;
  const int reinit_ret = mgr->ensure_initialized();
  mode.mini_mode_ = original_mini_mode;
  EXPECT_EQ(OB_SUCCESS, reinit_ret);
  EXPECT_EQ(capacity, mgr->get_map().bucket_count());
  EXPECT_TRUE(mgr->get_map().created());
  EXPECT_EQ(OB_SUCCESS, mgr->check_registered_checkers_drained());
}

TEST(InterruptRegistryLifecycle, SurvivingHostCheckerReleasesEveryRegistration)
{
  auto *mgr = ObGlobalInterruptManager::getInstance();
  ASSERT_NE(nullptr, mgr);
  ASSERT_EQ(OB_SUCCESS, mgr->ensure_initialized());
  constexpr int cycles = 3;
  std::promise<void> begin[cycles];
  std::promise<void> registered[cycles];
  std::promise<void> resume[cycles];
  std::promise<void> completed[cycles];
  int register_ret[cycles] = {};
  bool initially_clean[cycles] = {};
  bool received_interrupt[cycles] = {};
  ObInterruptChecker *checker_addr[cycles] = {};
  const int64_t capacity = mgr->get_map().bucket_count();
  std::thread host([&] {
    for (int cycle = 0; cycle < cycles; ++cycle) {
      begin[cycle].get_future().wait();
      const ObInterruptibleTaskID tid(0x1FEC7000 + cycle, 0x20261012);
      checker_addr[cycle] = get_checker();
      initially_clean[cycle] = !IS_INTERRUPTED();
      register_ret[cycle] = SET_INTERRUPTABLE(tid);
      registered[cycle].set_value();
      resume[cycle].get_future().wait();
      received_interrupt[cycle] = IS_INTERRUPTED();
      if (OB_SUCCESS == register_ret[cycle]) {
        UNSET_INTERRUPTABLE(tid);
      }
      completed[cycle].set_value();
    }
  });
  // No fatal assertion until every registration has been released and the
  // host joined, including a failed expectation or registration.
  for (int cycle = 0; cycle < cycles; ++cycle) {
    SCOPED_TRACE(cycle);
    const ObInterruptibleTaskID tid(0x1FEC7000 + cycle, 0x20261012);
    begin[cycle].set_value();
    registered[cycle].get_future().wait();
    EXPECT_EQ(OB_SUCCESS, register_ret[cycle]);
    EXPECT_TRUE(initially_clean[cycle]);
    EXPECT_EQ(OB_EAGAIN, mgr->check_registered_checkers_drained());
    EXPECT_EQ(OB_SUCCESS, mgr->ensure_initialized());
    ObInterruptCode code(OB_CANCELED);
    EXPECT_EQ(OB_SUCCESS, mgr->interrupt(tid, code));
    resume[cycle].set_value();
    completed[cycle].get_future().wait();
    EXPECT_TRUE(received_interrupt[cycle]);
    EXPECT_EQ(checker_addr[0], checker_addr[cycle]);
    EXPECT_EQ(OB_SUCCESS, mgr->check_registered_checkers_drained());
    EXPECT_EQ(0, mgr->get_map().size());
    EXPECT_EQ(capacity, mgr->get_map().bucket_count());
  }
  host.join();
}
