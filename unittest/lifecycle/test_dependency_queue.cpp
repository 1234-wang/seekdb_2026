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

#include <gtest/gtest.h>
#include "share/rc/ob_server_runtime.h"
#include "sql/executor/ob_maintain_dependency_info_task.h"
#include "sql/engine/cmd/ob_load_data_utils.h"

TEST(SqlLifecycle, DependencyQueueReinitializesBothSetsAndPreservesDuplicateInit)
{
  using namespace oceanbase::common;
  oceanbase::sql::ObMaintainDepInfoTaskQueue queue;
  queue.set_run_wrapper(oceanbase::share::server_runtime());
  ASSERT_EQ(OB_INVALID_ARGUMENT, queue.init(0, 8));
  for (int cycle = 0; cycle != 3; ++cycle) {
    SCOPED_TRACE(cycle);
    queue.set_run_wrapper(oceanbase::share::server_runtime());
    ASSERT_EQ(OB_SUCCESS, queue.init(1, 8));
    // IDs stored during the preceding database lifetime must not survive.
    EXPECT_EQ(OB_HASH_NOT_EXIST, queue.read_consistent_sys_view_from_set(2));
    ASSERT_EQ(OB_SUCCESS, queue.add_view_id_to_set(1));
    ASSERT_EQ(OB_SUCCESS, queue.add_consistent_sys_view_id_to_set(2));
    queue.set_last_execute_time(123);
    EXPECT_EQ(OB_INIT_TWICE, queue.init(1, 8));
    EXPECT_EQ(123, queue.get_last_execute_time());
    EXPECT_EQ(OB_HASH_EXIST, queue.read_consistent_sys_view_from_set(2));
    EXPECT_EQ(OB_SUCCESS, queue.erase_view_id_from_set(1));
    ASSERT_EQ(OB_SUCCESS, queue.start());
    queue.stop();
    queue.wait();
    ASSERT_EQ(OB_SUCCESS, queue.destroy());
    EXPECT_EQ(OB_SUCCESS, queue.destroy());
    EXPECT_EQ(nullptr, queue.get_root_command_service());
    EXPECT_EQ(0, queue.get_last_execute_time());
  }
}

TEST(SqlLifecycle, LoadDataRegistryReinitializesWithoutOwningJobStatistics)
{
  using namespace oceanbase::common;
  using namespace oceanbase::sql;
  ObGlobalLoadDataStatMap jobs;
  int64_t previous_id = 0;
  for (int cycle = 0; cycle != 3; ++cycle) {
    SCOPED_TRACE(cycle);
    ASSERT_EQ(OB_SUCCESS, jobs.init());
    ObLoadDataGID id;
    ObLoadDataGID::generate_new_id(id);
    EXPECT_GT(id.id, previous_id);
    previous_id = id.id;
    ObLoadDataStat status;
    status.job_id_ = id.id;
    status.total_bytes_ = 42;
    ASSERT_EQ(OB_SUCCESS, jobs.register_job(id, &status));
    EXPECT_EQ(OB_INIT_TWICE, jobs.init());
    EXPECT_EQ(OB_EAGAIN, jobs.destroy());
    ObLoadDataStat *borrowed = nullptr;
    ASSERT_EQ(OB_SUCCESS, jobs.get_job_status(id, borrowed));
    ASSERT_EQ(&status, borrowed);
    EXPECT_EQ(1, status.get_ref_cnt());
    ObLoadDataStat *owner = nullptr;
    ASSERT_EQ(OB_SUCCESS, jobs.unregister_job(id, owner));
    EXPECT_EQ(&status, owner);
    ASSERT_EQ(OB_SUCCESS, jobs.destroy());
    // Registry cleanup must not release or delete statistics owned by the job.
    EXPECT_EQ(1, status.get_ref_cnt());
    EXPECT_EQ(42, borrowed->total_bytes_);
    borrowed->release();
    EXPECT_EQ(0, status.get_ref_cnt());
    EXPECT_EQ(OB_SUCCESS, jobs.destroy());
    borrowed = nullptr;
    EXPECT_EQ(OB_NOT_INIT, jobs.get_job_status(id, borrowed));
    EXPECT_EQ(nullptr, borrowed);
  }
}
