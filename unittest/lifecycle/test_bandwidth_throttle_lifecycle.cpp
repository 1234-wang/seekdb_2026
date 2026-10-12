/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <chrono>
#include <future>
#include <thread>
#include <gtest/gtest.h>
#include "lib/utility/utility.h"
#include "lib/time/ob_time_utility.h"

using namespace oceanbase::common;

TEST(BandwidthThrottleLifecycle, StopInterruptsLongOutputReservation)
{
  ObInOutBandwidthThrottle throttle;
  ASSERT_EQ(OB_SUCCESS, throttle.init(1000));
  bool stopped = false;
  std::promise<void> entered;
  std::future<void> entered_future = entered.get_future();
  std::promise<int> completed;
  std::future<int> completed_future = completed.get_future();
  int64_t requested_sleep_us = -1;
  std::thread producer([&] {
    entered.set_value();
    // A five-second reservation must not delay shutdown for five seconds.
    completed.set_value(throttle.limit_out_and_sleep(
        5000, ObTimeUtility::current_time(), 10 * 1000 * 1000,
        &requested_sleep_us, &stopped));
  });
  entered_future.wait();
  EXPECT_EQ(std::future_status::timeout,
      completed_future.wait_for(std::chrono::milliseconds(30)));
  const int64_t stopping_time = ObTimeUtility::current_monotonic_time();
  ATOMIC_STORE(&stopped, true);
  EXPECT_EQ(std::future_status::ready,
      completed_future.wait_for(std::chrono::milliseconds(500)));
  producer.join();
  EXPECT_LT(ObTimeUtility::current_monotonic_time() - stopping_time, 500 * 1000);
  EXPECT_EQ(OB_IN_STOP_STATE, completed_future.get());
  // The wrapper retains its existing output-on-success convention.
  EXPECT_EQ(-1, requested_sleep_us);
}

TEST(BandwidthThrottleLifecycle, UnstoppedTokenWaitsForTheReservedDeadline)
{
  ObBandwidthThrottle throttle;
  ASSERT_EQ(OB_SUCCESS, throttle.init(1000));
  bool stopped = false;
  int64_t sleep_us = 0;
  const int64_t begin = ObTimeUtility::current_time();
  ASSERT_EQ(OB_SUCCESS, throttle.limit_and_sleep(200, begin, 5 * 1000 * 1000, sleep_us, &stopped));
  const int64_t elapsed = ObTimeUtility::current_time() - begin;
  EXPECT_GE(elapsed, 150 * 1000);
  EXPECT_GE(sleep_us, 150 * 1000);
  EXPECT_LT(elapsed, 1000 * 1000);
  // Polling must not repeatedly charge the same bytes while sleeping.
  ASSERT_EQ(OB_SUCCESS, throttle.limit_and_sleep(
      0, ObTimeUtility::current_time(), 5 * 1000 * 1000, sleep_us, &stopped));
  EXPECT_LT(sleep_us, 20 * 1000);
}

TEST(BandwidthThrottleLifecycle, DefaultOutputWaitStillAppliesTheLimit)
{
  ObInOutBandwidthThrottle throttle;
  ASSERT_EQ(OB_SUCCESS, throttle.init(1000));
  int64_t sleep_us = 0;
  const int64_t begin = ObTimeUtility::current_time();
  ASSERT_EQ(OB_SUCCESS, throttle.limit_out_and_sleep(100, begin, 5 * 1000 * 1000, &sleep_us));
  EXPECT_GE(ObTimeUtility::current_time() - begin, 75 * 1000);
  EXPECT_GE(sleep_us, 75 * 1000);
}

TEST(BandwidthThrottleLifecycle, DefaultWaitKeepsTheUint32Boundary)
{
  ObBandwidthThrottle throttle;
  ASSERT_EQ(OB_SUCCESS, throttle.init(0));
  int64_t sleep_us = -1;
  const int64_t max_idle_us = static_cast<int64_t>(UINT32_MAX) + 1000 * 1000;
  // The legacy path skips waits outside the range of its single usleep call.
  ASSERT_EQ(OB_SUCCESS, throttle.limit_and_sleep(
      1, ObTimeUtility::current_time(), max_idle_us, sleep_us));
  EXPECT_EQ(0, sleep_us);
}

TEST(BandwidthThrottleLifecycle, AlreadyStoppedTokenDoesNotReserveBytes)
{
  ObBandwidthThrottle throttle;
  ASSERT_EQ(OB_SUCCESS, throttle.init(1000));
  bool stopped = true;
  int64_t sleep_us = -1;
  EXPECT_EQ(OB_IN_STOP_STATE, throttle.limit_and_sleep(
      200, ObTimeUtility::current_time(), 5 * 1000 * 1000, sleep_us, &stopped));
  EXPECT_EQ(0, sleep_us);
  ATOMIC_STORE(&stopped, false);
  ASSERT_EQ(OB_SUCCESS, throttle.limit_and_sleep(
      0, ObTimeUtility::current_time(), 5 * 1000 * 1000, sleep_us, &stopped));
  EXPECT_LT(sleep_us, 20 * 1000);
}
