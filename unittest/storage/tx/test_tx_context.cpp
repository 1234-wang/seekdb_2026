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
#include <cstdio>
#include <cstring>
#include "storage/tx/ob_tx_ctx.h"

namespace oceanbase
{
namespace transaction
{

TEST(TestTxContext, retained_memory_budget)
{
  // Leave room for ordinary context fields, but catch a multi-KiB recorder
  // returning to either the participant base or the scheduler descriptor.
  EXPECT_LT(sizeof(ObTransCtx), 2 * 1024U);
  EXPECT_LT(sizeof(ObTxDesc), 4 * 1024U);
  printf("sizeof(ObTransCtx)=%zu sizeof(ObTxCtx)=%zu sizeof(ObTxDesc)=%zu\n",
         sizeof(ObTransCtx), sizeof(ObTxCtx), sizeof(ObTxDesc));
}

TEST(TestTxContext, failed_initialization_can_be_reused_for_replay_and_execution)
{
  ObTxCtx ctx;
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(common::OB_INVALID_ARGUMENT,
        ctx.init(0, ObTransID(), 1, nullptr, nullptr, i % 2 == 0, TxCtxSource::REPLAY));
    EXPECT_FALSE(ctx.is_inited());
    ctx.destroy();
    EXPECT_FALSE(ctx.is_inited());
  }
}

TEST(TestTxContext, descriptor_state_dump_preserves_session_and_transaction_state)
{
  ObTxDesc tx;
  tx.set_sessid(1492);
  char buf[16 * 1024] = {};
  const int64_t len = tx.to_string(buf, sizeof(buf));
  ASSERT_GT(len, 0);
  ASSERT_LT(len, sizeof(buf));
  EXPECT_NE(nullptr, strstr(buf, "session_id"));
  EXPECT_NE(nullptr, strstr(buf, "1492"));
  EXPECT_NE(nullptr, strstr(buf, "state"));
  EXPECT_NE(nullptr, strstr(buf, "tx_id"));
  // These entry points now dump state without an event recorder.
  tx.dump_state();
  tx.try_dump_state();
}

} // namespace transaction
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
