/**
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

#include "sql/engine/expr/ob_expr_uuid.h"
#include <gtest/gtest.h>
#include <string.h>

namespace oceanbase
{
namespace sql
{

class TestUUIDNode : public ::testing::Test
{
protected:
  static void SetUpTestCase() {}
  static void TearDownTestCase() {}
};

/**
 * Simulate an environment where the interface lookup is denied, e.g. a sandbox
 * where ioctl(SIOCGIFCONF) fails with EPERM, see issue #1480.
 */
static int denied_mac_finder(unsigned char *mac_addr)
{
  (void)mac_addr;
  return OB_ERR_UNEXPECTED;
}

TEST_F(TestUUIDNode, init_succeeds_when_no_mac_address_is_available)
{
  ObUUIDNode::MacFinder saved_finder = ObUUIDNode::mac_finder_;
  ObUUIDNode::mac_finder_ = denied_mac_finder;
  ObUUIDNode node;
  ObUUIDNode::mac_finder_ = saved_finder;

  ASSERT_TRUE(node.is_inited_);
  // RFC 4122 section 4.5: the multicast bit marks the node id as a random one
  ASSERT_NE(0, node.mac_addr_[0] & 0x01);
}

TEST_F(TestUUIDNode, gen_random_node_id_marks_the_multicast_bit)
{
  unsigned char node_id[6] = {0};
  ASSERT_EQ(OB_SUCCESS, ObUUIDNode::gen_random_node_id(node_id));
  ASSERT_NE(0, node_id[0] & 0x01);

  unsigned char another_node_id[6] = {0};
  ASSERT_EQ(OB_SUCCESS, ObUUIDNode::gen_random_node_id(another_node_id));
  ASSERT_NE(0, memcmp(node_id, another_node_id, sizeof(node_id)));
}

TEST_F(TestUUIDNode, gen_random_node_id_rejects_a_null_buffer)
{
  ASSERT_EQ(OB_ERR_UNEXPECTED, ObUUIDNode::gen_random_node_id(NULL));
}

TEST_F(TestUUIDNode, find_mac_address_does_not_return_a_partial_address)
{
  unsigned char mac_addr[6] = {0};
  int ret = ObUUIDNode::find_mac_address(mac_addr);
  if (OB_SUCC(ret)) {
    bool all_zero = true;
    for (int i = 0; i < 6; ++i) {
      if (mac_addr[i] != 0) {
        all_zero = false;
        break;
      }
    }
    ASSERT_FALSE(all_zero);
  }
}

} // namespace sql
} // namespace oceanbase
