/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <gtest/gtest.h>
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/task/ob_timer.h"
#include "share/ob_timezone_mgr.h"

using namespace oceanbase::common;
using namespace oceanbase::omt;

TEST(TimezoneLifecycle, BorrowedMapSurvivesShutdownAndAllowsRetry)
{
  ObTimezoneMgr &manager = ObTimezoneMgr::get_instance();
  ASSERT_EQ(OB_SUCCESS, manager.destroy());
  ASSERT_EQ(OB_SUCCESS, ObTimerService::get_instance().start());
  ObMySQLProxy proxy;
  ASSERT_EQ(OB_SUCCESS, proxy.init(false));
  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE(cycle);
    ASSERT_EQ(OB_SUCCESS, manager.init(proxy));
    EXPECT_EQ(OB_INIT_TWICE, manager.init(proxy));
    ObTZMapWrap first;
    ObTimeZoneInfoManager *info_manager = nullptr;
    ASSERT_EQ(OB_SUCCESS, manager.get_timezone(first, info_manager));
    ASSERT_NE(nullptr, info_manager);
    const ObTZInfoMap *map = first.get_tz_map();
    ASSERT_NE(nullptr, map);
    {
      ObTZMapWrap copied(first);
      ObTZMapWrap assigned;
      assigned = copied;
      EXPECT_EQ(3, map->borrowed_maps());
      ASSERT_EQ(OB_EAGAIN, manager.destroy());
      EXPECT_TRUE(map->inited_);
      EXPECT_EQ(-1, info_manager->get_version());
      ObTZMapWrap unavailable;
      ObTimeZoneInfoManager *unavailable_manager = info_manager;
      EXPECT_EQ(OB_NOT_INIT, manager.get_timezone(unavailable, unavailable_manager));
      EXPECT_EQ(nullptr, unavailable.get_tz_map());
      EXPECT_EQ(nullptr, unavailable_manager);
    } // Copy and assignment each release their map reference in the destructor.
    EXPECT_EQ(1, map->borrowed_maps());
    EXPECT_EQ(OB_EAGAIN, manager.destroy());
    first.set_tz_map(nullptr);
    ASSERT_EQ(OB_SUCCESS, manager.destroy());
    EXPECT_EQ(OB_SUCCESS, manager.destroy());
    EXPECT_FALSE(manager.is_inited());
    EXPECT_FALSE(manager.is_usable());
  }
  ObTimerService::get_instance().destroy();
}
