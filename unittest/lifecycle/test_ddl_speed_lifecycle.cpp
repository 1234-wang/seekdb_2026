/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <gtest/gtest.h>
#include "storage/ddl/ob_ddl_redo_log_writer.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

namespace
{
class UnexpectedChecker : public ObDDLNeedStopWriteChecker
{
public:
  bool check_need_stop_write() override
  {
    ADD_FAILURE() << "A rejected limiter must not reach its storage checker";
    return false;
  }
};

class ScopedDdlSpeed
{
public:
  ~ScopedDdlSpeed()
  {
    ObDDLCtrlSpeedHandle::get_instance().stop();
    EXPECT_EQ(OB_SUCCESS, ObDDLCtrlSpeedHandle::get_instance().destroy());
    timer.destroy();
    ObTimerService::get_instance().destroy();
  }
  ObTimer timer;
};
}

TEST(DdlSpeedLifecycle, TimerFailureRollsBackAndShutdownAllowsReinitialization)
{
  ScopedDdlSpeed scoped;
  ObDDLCtrlSpeedHandle &speed = ObDDLCtrlSpeedHandle::get_instance();
  ASSERT_EQ(OB_SUCCESS, speed.destroy());
  // A failed schedule must not retain the pointer to an unusable timer.
  EXPECT_EQ(OB_NOT_INIT, speed.init(scoped.timer));
  ASSERT_EQ(OB_SUCCESS, ObTimerService::get_instance().start());
  ASSERT_EQ(OB_SUCCESS, scoped.timer.init("DdlLifecycle"));
  UnexpectedChecker checker;
  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE(cycle);
    ASSERT_EQ(OB_SUCCESS, speed.init(scoped.timer));
    EXPECT_EQ(OB_INIT_TWICE, speed.init(scoped.timer));
    int64_t sleep_us = -1;
    EXPECT_EQ(OB_INVALID_ARGUMENT, speed.limit_and_sleep(-1, 1, checker, sleep_us));
    speed.stop();
    EXPECT_EQ(OB_IN_STOP_STATE, speed.limit_and_sleep(0, 1, checker, sleep_us));
    ASSERT_EQ(OB_SUCCESS, speed.destroy());
    EXPECT_EQ(OB_SUCCESS, speed.destroy());
    EXPECT_EQ(OB_NOT_INIT, speed.limit_and_sleep(0, 1, checker, sleep_us));
  }
}
