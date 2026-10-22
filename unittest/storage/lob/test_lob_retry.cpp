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

#include "storage/lob/ob_lob_retry.h"
#include <gtest/gtest.h>
#include <cstddef>
#include <cstring>

namespace oceanbase
{
namespace storage
{
using namespace common;

TEST(LobRetry, ignores_nonretryable_errors_even_after_deadline)
{
  ObLobAccessParam param;
  param.timeout_ = 0;
  bool need_retry = true;
  EXPECT_EQ(OB_SUCCESS, ObLobRetryUtil::check_need_retry(param, OB_ERR_UNEXPECTED, 0, need_retry));
  EXPECT_FALSE(need_retry);
  EXPECT_EQ(OB_TIMEOUT, ObLobRetryUtil::check_need_retry(param, OB_TABLET_NOT_EXIST, 0, need_retry));
  EXPECT_FALSE(need_retry);
}

TEST(LobRetry, local_access_without_locator_retains_retry_policy)
{
  ObLobAccessParam param;
  param.timeout_ = ObTimeUtility::current_time() + 10000000;
  const int errors[] = {OB_LS_NOT_EXIST, OB_REPLICA_NOT_READABLE, OB_NOT_MASTER,
                        OB_NO_READABLE_REPLICA, OB_TABLET_NOT_EXIST, OB_LS_OFFLINE};
  for (int error : errors) {
    bool need_retry = false;
    EXPECT_EQ(OB_SUCCESS, ObLobRetryUtil::check_need_retry(param, error, 0, need_retry));
    EXPECT_TRUE(need_retry) << error;
  }
}

TEST(LobRetry, locator_tablet_identity_is_validated_before_retry)
{
  char buffer[1024] = {};
  ObLobLocatorV2 locator;
  locator.assign_buffer(buffer, sizeof(buffer));
  ObMemLobExternFlags flags(false);
  flags.has_access_info_ = true;
  flags.has_retry_info_ = true;
  ObLobCommon disk_locator;
  ASSERT_EQ(OB_SUCCESS, locator.fill(PERSISTENT_LOB, flags, ObString(), &disk_locator,
                                    sizeof(disk_locator), sizeof(disk_locator), 0, false));
  ASSERT_EQ(OB_SUCCESS, locator.set_access_info(LobAccessInfo(200001, CS_TYPE_UTF8MB4_BIN)));
  ObLobAccessParam param;
  param.timeout_ = ObTimeUtility::current_time() + 10000000;
  param.lob_locator_ = &locator;
  param.tablet_id_ = ObTabletID(200001);
  bool need_retry = false;
  EXPECT_EQ(OB_SUCCESS, ObLobRetryUtil::check_need_retry(param, OB_NOT_MASTER, 0, need_retry));
  EXPECT_TRUE(need_retry);
  param.tablet_id_ = ObTabletID(200002);
  EXPECT_EQ(OB_ERR_UNEXPECTED, ObLobRetryUtil::check_need_retry(param, OB_NOT_MASTER, 0, need_retry));
  EXPECT_FALSE(need_retry);
  // With no retry metadata, the local policy skips locator validation.
  ObMemLobExternHeader *header = nullptr;
  ASSERT_EQ(OB_SUCCESS, locator.get_extern_header(header));
  header->flags_.has_retry_info_ = false;
  EXPECT_EQ(OB_SUCCESS, ObLobRetryUtil::check_need_retry(param, OB_NOT_MASTER, 0, need_retry));
  EXPECT_TRUE(need_retry);
}

struct LegacyLobAccessInfo
{
  int64_t tablet_id_;
  ObCollationType cid_;
};

struct LegacyLobRetryInfo
{
  int64_t is_select_leader_ : 1;
  int64_t read_latest_ : 1;
  int64_t reserved_ : 62;
  ObAddr addr_;
  uint64_t timeout_;
};

TEST(LobRetry, retained_payload_layout_reads_legacy_retry_metadata)
{
  EXPECT_EQ(sizeof(LegacyLobAccessInfo), sizeof(LobAccessInfo));
  EXPECT_EQ(alignof(LegacyLobAccessInfo), alignof(LobAccessInfo));
  EXPECT_EQ(offsetof(LegacyLobAccessInfo, tablet_id_), offsetof(LobAccessInfo, tablet_id_));
  EXPECT_EQ(offsetof(LegacyLobAccessInfo, cid_), offsetof(LobAccessInfo, cid_));
  ASSERT_EQ(sizeof(LegacyLobRetryInfo), sizeof(ObMemLobRetryInfo));
  EXPECT_EQ(offsetof(LegacyLobRetryInfo, addr_), offsetof(ObMemLobRetryInfo, reserved_addr_));
  EXPECT_EQ(offsetof(LegacyLobRetryInfo, timeout_), offsetof(ObMemLobRetryInfo, reserved_timeout_));
  LegacyLobRetryInfo legacy;
  legacy.is_select_leader_ = false;
  legacy.read_latest_ = true;
  legacy.reserved_ = 0;
  ASSERT_TRUE(legacy.addr_.set_ip_addr("127.0.0.1", 2881));
  legacy.timeout_ = 123456789;
  ObMemLobRetryInfo restored;
  std::memcpy(&restored, &legacy, sizeof(restored));
  EXPECT_TRUE(restored.read_latest_);
  EXPECT_EQ(legacy.addr_, restored.reserved_addr_);
  EXPECT_EQ(legacy.timeout_, restored.reserved_timeout_);
}

} // namespace storage
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
