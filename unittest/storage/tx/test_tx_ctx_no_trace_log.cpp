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

// Regression test for Issue #1492:
// "Remove per-transaction TraceLog state and obsolete tracing plumbing".
//
// Every transaction context previously embedded an ObTransTraceLog recorder
// (approximately 4.5 KiB per context: event storage + information buffer + lock
// overhead). The recorder was constructed, reset and included in the size of
// every ObTransCtx/ObTxDesc even when the feature was disabled.
//
// The dedicated per-transaction TraceLog machinery has been removed entirely.
// These tests guard against its reintroduction:
//  1. Compile-time bounds on sizeof(ObTransCtx)/sizeof(ObTxDesc). On x86_64
//     Linux the pre-fix layouts were
//       sizeof(ObTransCtx) >= sizeof(ObTransTraceLog) (4544 B) + remaining members
//       sizeof(ObTxDesc)   >= sizeof(ObTransTraceLog) (4544 B) + remaining members
//     so both bounds below are violated by the pre-fix code and satisfied with
//     comfortable margin by the post-fix layouts.
//  2. Lifecycle probes on the public transaction APIs that were touched by the
//     removal (init/destroy paths of the tx ctx, and the tx descriptor state
//     machine) to show the behavior is preserved.

#include <gtest/gtest.h>
#include "storage/tx/ob_trans_ctx.h"
#include "storage/tx/ob_trans_define_v4.h"
#include "storage/tx/ob_tx_ctx.h"

namespace oceanbase
{
namespace transaction
{

static_assert(sizeof(ObTransCtx) < 4096,
              "ObTransCtx must not embed the per-transaction TraceLog recorder (Issue #1492)");
static_assert(sizeof(ObTxDesc) < 2048,
              "ObTxDesc must not embed the per-transaction TraceLog recorder (Issue #1492)");

class TestTxCtxNoTraceLog : public ::testing::Test
{
protected:
  void SetUp() override
  {
    // A deliberately unattached context: the public init path must still reject
    // invalid input (same probe as the replay executor test) without any
    // TraceLog plumbing. init() is safe to call repeatedly: it resets the
    // context first and returns OB_INVALID_ARGUMENT deterministically.
    ASSERT_EQ(OB_INVALID_ARGUMENT,
              ctx_.init(0, ObTransID(), 1, nullptr, nullptr, true, TxCtxSource::REPLAY));
  }

  ObTxCtx ctx_;
};

TEST_F(TestTxCtxNoTraceLog, lifecycle_init_rejects_invalid_input)
{
  // SetUp already verified init(); a second call must behave identically and
  // must not crash or leave the context half-initialized.
  ASSERT_EQ(OB_INVALID_ARGUMENT,
            ctx_.init(0, ObTransID(), 1, nullptr, nullptr, true, TxCtxSource::REPLAY));
}

TEST_F(TestTxCtxNoTraceLog, tx_desc_starts_idle)
{
  ObTxDesc desc;
  EXPECT_FALSE(desc.is_in_tx());
  EXPECT_FALSE(desc.is_tx_active());
  EXPECT_FALSE(desc.need_rollback());
  // The public state probes must not depend on any per-transaction recorder.
  EXPECT_FALSE(desc.in_tx_or_has_extra_state());
}

TEST_F(TestTxCtxNoTraceLog, tx_desc_state_string_is_available)
{
  ObTxDesc desc;
  const ObString &state_str = desc.get_tx_state_str();
  EXPECT_TRUE(state_str.length() > 0);
}

} // namespace transaction
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
