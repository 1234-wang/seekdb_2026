/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <string>
#include <gtest/gtest.h>
#include "common/object/ob_object.h"
#include "common/timezone/ob_time_convert.h"
#include "sql/das/ob_das_dml_ctx_define.h"

using namespace oceanbase;

namespace
{
void compare_timestamp_output(const common::ObTimeZoneInfo &legacy,
                              const common::ObTimeZoneInfo &snapshot)
{
  common::ObObj timestamp;
  timestamp.set_timestamp(1609459200000000LL);
  char legacy_buf[256] = {};
  char snapshot_buf[256] = {};
  int64_t legacy_pos = 0;
  int64_t snapshot_pos = 0;
  // This is the exact printer used by extract_rowkey for duplicate-key errors.
  ASSERT_EQ(common::OB_SUCCESS, timestamp.print_plain_str_literal(
      legacy_buf, sizeof(legacy_buf), legacy_pos, &legacy));
  ASSERT_EQ(common::OB_SUCCESS, timestamp.print_plain_str_literal(
      snapshot_buf, sizeof(snapshot_buf), snapshot_pos, &snapshot));
  EXPECT_EQ(std::string(legacy_buf, legacy_pos), std::string(snapshot_buf, snapshot_pos));

  common::ObOTimestampData timestamp_ltz;
  timestamp_ltz.time_us_ = 1609459200000000LL;
  timestamp_ltz.time_ctx_.set_offset_min(480);
  common::ObDataTypeCastParams legacy_params(&legacy);
  common::ObDataTypeCastParams snapshot_params(&snapshot);
  legacy_pos = 0;
  snapshot_pos = 0;
  ASSERT_EQ(common::OB_SUCCESS, common::ObTimeConverter::otimestamp_to_str(
      timestamp_ltz, legacy_params, 6, common::ObTimestampLTZType,
      legacy_buf, sizeof(legacy_buf), legacy_pos));
  ASSERT_EQ(common::OB_SUCCESS, common::ObTimeConverter::otimestamp_to_str(
      timestamp_ltz, snapshot_params, 6, common::ObTimestampLTZType,
      snapshot_buf, sizeof(snapshot_buf), snapshot_pos));
  EXPECT_EQ(std::string(legacy_buf, legacy_pos), std::string(snapshot_buf, snapshot_pos));
}
}

TEST(DmlTimezoneSnapshot, OffsetValueNeedsNoMapWhenArenaIsDiscarded)
{
  common::ObTZInfoMap map;
  ASSERT_EQ(common::OB_SUCCESS, map.init(lib::ObMemAttr("TestDmlTZ")));
  common::ObArenaAllocator arena("TestDmlTZ");
  for (const int32_t offset : {28800, -18000, 0}) {
    common::ObTimeZoneInfo source;
    source.set_offset(offset);
    source.set_error_on_overlap_time(true);
    source.set_tz_info_map(&map);
    ASSERT_EQ(1, map.borrowed_maps());
    void *buffer = arena.alloc(sizeof(sql::ObDASInsCtDef));
    ASSERT_NE(nullptr, buffer);
    auto *definition = new (buffer) sql::ObDASInsCtDef(arena);
    ASSERT_EQ(common::OB_SUCCESS, definition->tz_info_.assign(source));
    EXPECT_EQ(1, map.borrowed_maps());
    EXPECT_EQ(nullptr, definition->tz_info_.get_tz_info_map());
    EXPECT_EQ(offset, definition->tz_info_.get_offset());
    EXPECT_TRUE(definition->tz_info_.is_error_on_overlap_time());
    {
      const common::ObTimeZoneInfo legacy(source);
      compare_timestamp_output(legacy, definition->tz_info_);
    }
    // Also models a compilation failure before the definition is attached to a
    // specification tree: freeing the arena must leave no timezone ownership.
    arena.reset();
    source.set_tz_info_map(nullptr);
    EXPECT_EQ(0, map.borrowed_maps());
  }
  EXPECT_EQ(common::OB_SUCCESS, map.destroy());
}

TEST(DmlTimezoneSnapshot, PositionalSourceKeepsTheExistingBaseSliceSemantics)
{
  common::ObTZInfoMap map;
  ASSERT_EQ(common::OB_SUCCESS, map.init(lib::ObMemAttr("TestNamedTZ")));
  common::ObArenaAllocator arena("TestNamedTZ");
  {
    common::ObTimeZoneInfoPos source;
    source.set_tz_id(123);
    ASSERT_EQ(common::OB_SUCCESS, source.set_tz_name("Asia/Shanghai", 13));
    source.set_tz_info_map(&map);
    source.set_error_on_overlap_time(true);
    common::ObTZTransitionTypeInfo default_type;
    default_type.info_.offset_sec_ = 28800;
    ASSERT_EQ(common::OB_SUCCESS, source.set_default_tran_type(default_type));
    void *buffer = arena.alloc(sizeof(sql::ObDASLockCtDef));
    ASSERT_NE(nullptr, buffer);
    auto *definition = new (buffer) sql::ObDASLockCtDef(arena);
    const common::ObTimeZoneInfo &base = source;
    ASSERT_EQ(common::OB_SUCCESS, definition->tz_info_.assign(base));
    EXPECT_EQ(1, map.borrowed_maps());
    EXPECT_EQ(nullptr, definition->tz_info_.get_tz_info_map());
    {
      // The old implicit copy already sliced positional transition information
      // into a base value. This patch preserves that behavior, without claiming
      // to add named-zone/DST support to the compiled scalar snapshot.
      const common::ObTimeZoneInfo legacy(base);
      EXPECT_EQ(legacy.get_offset(), definition->tz_info_.get_offset());
      EXPECT_EQ(legacy.get_tz_id(), definition->tz_info_.get_tz_id());
      compare_timestamp_output(legacy, definition->tz_info_);
    }
    arena.reset();
  }
  EXPECT_EQ(0, map.borrowed_maps());
  EXPECT_EQ(common::OB_SUCCESS, map.destroy());
}
