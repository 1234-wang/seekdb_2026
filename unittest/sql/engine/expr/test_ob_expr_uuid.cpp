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

#include "sql/engine/expr/ob_expr_uuid.h"

#include <gtest/gtest.h>

namespace oceanbase
{
namespace sql
{

TEST(ObUUIDNodeTest, UsesRandomMulticastNodeWhenMacDiscoveryIsUnavailable)
{
  ObUUIDNode node(false);
  EXPECT_FALSE(node.is_inited_);

  ASSERT_EQ(common::OB_SUCCESS, node.init(true));
  EXPECT_TRUE(node.is_inited_);
  EXPECT_EQ(1, node.mac_addr_[0] & 0x01);
  EXPECT_NE(0, (node.mac_addr_[0] & 0xfe) | node.mac_addr_[1] | node.mac_addr_[2]
                   | node.mac_addr_[3] | node.mac_addr_[4] | node.mac_addr_[5]);
}

TEST(ObExprUuidTest, GeneratesServerUuidBeforeGlobalNodeInitialization)
{
  char uuid[UuidCommon::LENGTH_UUID] = {};
  ASSERT_EQ(common::OB_SUCCESS,
            ObExprUuid::gen_server_uuid(uuid, sizeof(uuid)));

  char binary[UuidCommon::BYTE_LENGTH] = {};
  bool is_valid = false;
  ASSERT_EQ(common::OB_SUCCESS,
            UuidCommon::uuid2bin(binary, is_valid, uuid, sizeof(uuid)));
  ASSERT_TRUE(is_valid);
  EXPECT_EQ(1, (static_cast<unsigned char>(binary[6]) >> 4) & 0x0f);
  EXPECT_EQ(2, (static_cast<unsigned char>(binary[8]) >> 6) & 0x03);
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
