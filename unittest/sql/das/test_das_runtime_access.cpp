/*
 * Copyright (c) 2025 OceanBase.
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

#include "sql/engine/ob_exec_context.h"
#include "sql/engine/px/ob_dfo.h"
#include <gtest/gtest.h>
#include <vector>

namespace oceanbase
{
namespace sql
{
using namespace common;

TEST(DASRuntimeAccess, preserve_retry_error_until_explicit_reset)
{
  ObArenaAllocator allocator;
  ObDASCtx ctx(allocator);
  ctx.record_retry_error(OB_TABLET_NOT_EXIST);
  // Successful cleanup and unrelated errors must not hide the error sent to PX.
  ctx.record_access_error(OB_SUCCESS);
  ctx.record_access_error(OB_ERR_UNEXPECTED);
  EXPECT_EQ(OB_TABLET_NOT_EXIST, ctx.get_retry_error());
  ctx.record_access_error(OB_LOCATION_NOT_EXIST);
  EXPECT_EQ(OB_LOCATION_NOT_EXIST, ctx.get_retry_error());
  ctx.record_retry_error(OB_SUCCESS);
  EXPECT_EQ(OB_SUCCESS, ctx.get_retry_error());
}

TEST(DASRuntimeAccess, virtual_table_validation_retains_retry_error)
{
  ObArenaAllocator allocator;
  ObDASCtx ctx(allocator);
  const uint64_t virtual_id = OB_MAX_SYS_TABLE_ID + 1;
  ASSERT_TRUE(is_virtual_table(virtual_id));
  EXPECT_EQ(OB_SUCCESS, ctx.validate_tablet_access(virtual_id, ObTabletID(1)));
  EXPECT_EQ(OB_SUCCESS, ctx.validate_tablet_access(virtual_id, ObTabletID(EMPTY_VIRTUAL_TABLE_TABLET_ID)));
  EXPECT_EQ(OB_LOCATION_NOT_EXIST, ctx.validate_tablet_access(virtual_id, ObTabletID(200001)));
  EXPECT_EQ(OB_LOCATION_NOT_EXIST, ctx.get_retry_error());
  EXPECT_EQ(OB_SUCCESS, ctx.validate_tablet_access(500001, ObTabletID(200001)));
  EXPECT_EQ(OB_LOCATION_NOT_EXIST, ctx.get_retry_error());
}

TEST(DASRuntimeAccess, aliases_related_tablets_and_px_roundtrip)
{
  ObArenaAllocator allocator;
  ObDASCtx ctx(allocator);
  DASTableAccessMeta meta(allocator);
  meta.table_ref_id_ = 600001;
  meta.ref_table_id_ = 500001;
  meta.related_table_ids_.set_capacity(1);
  ASSERT_EQ(OB_SUCCESS, meta.related_table_ids_.push_back(500002));
  ASSERT_EQ(OB_SUCCESS, ctx.get_related_tablet_map().add_related_tablet_id(
      ObTabletID(200001), 500002, ObTabletID(200002), 22, 2));
  DASRuntimeTableAccess *table = nullptr;
  DASRuntimeTabletAccess *tablet = nullptr;
  ASSERT_EQ(OB_SUCCESS, ctx.ensure_table_access(meta, table));
  ASSERT_EQ(OB_SUCCESS, ctx.ensure_tablet_access(*table, ObTabletID(200001), tablet, 11, 1));
  table->is_reading_ = true;
  table->is_writing_ = true;
  table->is_fk_check_ = true;
  DASRuntimeTabletAccess *same_tablet = nullptr;
  ASSERT_EQ(OB_SUCCESS, ctx.ensure_tablet_access(*table, ObTabletID(200001), same_tablet, 11, 1));
  EXPECT_EQ(tablet, same_tablet);
  ASSERT_NE(nullptr, tablet->next_);
  EXPECT_EQ(ObTabletID(200002), tablet->next_->tablet_id_);
  EXPECT_EQ(tablet, tablet->next_->next_);

  DASTableAccessMeta alias_meta(allocator);
  alias_meta.table_ref_id_ = 600002;
  alias_meta.ref_table_id_ = 500001;
  DASRuntimeTableAccess *alias_table = nullptr;
  ASSERT_EQ(OB_SUCCESS, ctx.ensure_table_access(alias_meta, alias_table));
  ASSERT_NE(table, alias_table);
  ASSERT_EQ(OB_SUCCESS, ctx.ensure_tablet_access(*alias_table, ObTabletID(200001), same_tablet, 11, 1));
  EXPECT_NE(tablet, same_tablet);

  std::vector<char> bytes(ctx.get_serialize_size());
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, ctx.serialize(bytes.data(), bytes.size(), pos));
  EXPECT_EQ(static_cast<int64_t>(bytes.size()), pos);
  ObArenaAllocator restored_allocator;
  ObDASCtx restored(restored_allocator);
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize(bytes.data(), bytes.size(), pos));
  EXPECT_EQ(static_cast<int64_t>(bytes.size()), pos);
  auto *restored_table = restored.find_table_access(600001, 500001);
  auto *restored_index = restored.find_table_access(600001, 500002);
  auto *restored_alias = restored.find_table_access(600002, 500001);
  ASSERT_NE(nullptr, restored_table);
  ASSERT_NE(nullptr, restored_index);
  ASSERT_NE(nullptr, restored_alias);
  EXPECT_TRUE(restored_table->is_reading_);
  EXPECT_TRUE(restored_table->is_writing_);
  EXPECT_TRUE(restored_table->is_fk_check_);
  auto *restored_tablet = restored_table->get_first_tablet_access();
  ASSERT_NE(nullptr, restored_tablet);
  EXPECT_EQ(11, restored_tablet->partition_id_);
  EXPECT_EQ(1, restored_tablet->first_level_part_id_);
  EXPECT_EQ(restored_index->get_first_tablet_access(), restored_tablet->next_);
  EXPECT_EQ(restored_tablet, restored_tablet->next_->next_);
  EXPECT_EQ(22, restored_tablet->next_->partition_id_);
  EXPECT_EQ(2, restored_tablet->next_->first_level_part_id_);
  EXPECT_NE(restored_tablet, restored_alias->get_first_tablet_access());
}

// Freeze the former V1 tablet payload independently of the runtime type names.
struct LegacyTabletV1
{
  OB_UNIS_VERSION(1);
public:
  ObTabletID tablet_id_;
  uint64_t flags_;
  uint64_t partition_id_;
  uint64_t first_level_part_id_;
};
OB_SERIALIZE_MEMBER(LegacyTabletV1, tablet_id_, flags_, partition_id_, first_level_part_id_);

TEST(DASRuntimeAccess, tablet_v1_wire_compatibility)
{
  LegacyTabletV1 legacy;
  legacy.tablet_id_ = ObTabletID(200001);
  legacy.flags_ = 1;
  legacy.partition_id_ = 11;
  legacy.first_level_part_id_ = 1;
  std::vector<char> bytes(legacy.get_serialize_size());
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, legacy.serialize(bytes.data(), bytes.size(), pos));
  DASRuntimeTabletAccess tablet;
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, tablet.deserialize(bytes.data(), bytes.size(), pos));
  EXPECT_EQ(legacy.tablet_id_, tablet.tablet_id_);
  EXPECT_EQ(legacy.flags_, tablet.reserved_flags_);
  EXPECT_EQ(legacy.partition_id_, tablet.partition_id_);
  EXPECT_EQ(legacy.first_level_part_id_, tablet.first_level_part_id_);
  std::vector<char> roundtrip(tablet.get_serialize_size());
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, tablet.serialize(roundtrip.data(), roundtrip.size(), pos));
  EXPECT_EQ(bytes, roundtrip);
}

// Preserve the old PX key layout, including the dynamic-DML flag.
struct LegacySqcKeyV1
{
  OB_UNIS_VERSION(1);
public:
  int64_t table_location_key_;
  int64_t ref_table_id_;
  ObTabletID tablet_id_;
  bool is_dml_;
  bool is_loc_uncertain_;
};
OB_SERIALIZE_MEMBER(LegacySqcKeyV1, table_location_key_, ref_table_id_, tablet_id_,
                    is_dml_, is_loc_uncertain_);

TEST(DASRuntimeAccess, px_key_v1_wire_compatibility)
{
  for (bool dynamic : {false, true}) {
    LegacySqcKeyV1 legacy;
    legacy.table_location_key_ = 600001;
    legacy.ref_table_id_ = 500001;
    legacy.tablet_id_ = ObTabletID(200001);
    legacy.is_dml_ = true;
    legacy.is_loc_uncertain_ = dynamic;
    std::vector<char> bytes(legacy.get_serialize_size());
    int64_t pos = 0;
    ASSERT_EQ(OB_SUCCESS, legacy.serialize(bytes.data(), bytes.size(), pos));
    SqcTabletAccessKey key;
    pos = 0;
    ASSERT_EQ(OB_SUCCESS, key.deserialize(bytes.data(), bytes.size(), pos));
    EXPECT_EQ(legacy.table_location_key_, key.table_ref_id_);
    EXPECT_EQ(legacy.ref_table_id_, key.ref_table_id_);
    EXPECT_EQ(legacy.tablet_id_, key.tablet_id_);
    EXPECT_TRUE(key.is_dml_);
    EXPECT_EQ(dynamic, key.has_dynamic_tablets_);
    std::vector<char> roundtrip(key.get_serialize_size());
    pos = 0;
    ASSERT_EQ(OB_SUCCESS, key.serialize(roundtrip.data(), roundtrip.size(), pos));
    EXPECT_EQ(bytes, roundtrip);
  }
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
