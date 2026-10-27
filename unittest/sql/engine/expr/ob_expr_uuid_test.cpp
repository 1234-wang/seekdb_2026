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

#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_uuid.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

class ObExprUuidTest : public ::testing::Test
{
public:
  ObExprUuidTest();
  virtual ~ObExprUuidTest();
  virtual void SetUp();
  virtual void TearDown();
private:
  // disallow copy
  ObExprUuidTest(const ObExprUuidTest &other);
  ObExprUuidTest& operator=(const ObExprUuidTest &other);
};

ObExprUuidTest::ObExprUuidTest()
{
}

ObExprUuidTest::~ObExprUuidTest()
{
}

void ObExprUuidTest::SetUp()
{
}

void ObExprUuidTest::TearDown()
{
}

// Regression test for issue #1480: uuid node initialization must not fail
// when no physical MAC address can be obtained (e.g. interface enumeration
// is denied in a restricted sandbox). ObUUIDNode::init() falls back to a
// random node with the RFC 4122 multicast bit set instead of returning
// OB_ERR_UNEXPECTED, so server startup is never blocked here.
TEST_F(ObExprUuidTest, uuid_node_init_always_succeeds)
{
  ObUUIDNode node;  // constructor calls init()
  ASSERT_TRUE(node.is_inited_);
  ASSERT_EQ(OB_SUCCESS, node.init());  // re-init must also succeed
  ASSERT_TRUE(node.is_inited_);
}

// The node must never be all zero, whether it came from a real MAC address
// or the random fallback.
TEST_F(ObExprUuidTest, uuid_node_not_all_zero)
{
  ObUUIDNode node;
  ASSERT_TRUE(node.is_inited_);
  bool all_zero = true;
  for (int i = 0; i < 6; ++i) {
    if (node.mac_addr_[i] != 0) {
      all_zero = false;
      break;
    }
  }
  ASSERT_FALSE(all_zero);
}

// The random fallback must set the RFC 4122 section 4.5 multicast bit, so a
// generated node can never collide with a real IEEE 802 MAC address.
TEST_F(ObExprUuidTest, random_node_sets_multicast_bit)
{
  ObUUIDNode node;
  ASSERT_EQ(OB_SUCCESS, node.init_random_node());
  ASSERT_EQ(0x01, node.mac_addr_[0] & 0x01);
  bool all_zero = true;
  for (int i = 0; i < 6; ++i) {
    if (node.mac_addr_[i] != 0) {
      all_zero = false;
      break;
    }
  }
  ASSERT_FALSE(all_zero);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
