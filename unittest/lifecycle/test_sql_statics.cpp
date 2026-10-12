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

#include <string>
#include <gtest/gtest.h>
#include "sql/ob_sql_init.h"
#include "storage/fts/dict/ob_gen_dic_loader.h"
#include "share/schema/ob_schema_mem_mgr.h"
#include "share/schema/ob_schema_mgr_cache.h"
#include "observer/ob_server_schema_updater.h"
#include "sql/session/ob_user_resource_mgr.h"

using namespace oceanbase;

TEST(SqlStaticsLifecycle, CompletedInitializationKeepsConstantsAndUuidClock)
{
  ASSERT_EQ(common::OB_SUCCESS, sql::init_sql_expr_static_var());
  const uint32_t *number_digits = common::ObNumberConstValue::MYSQL_MAX[10][2].get_digits();
  const auto *decimal_constant = wide::ObDecimalIntConstValue::get_max_value(10);
  ASSERT_NE(nullptr, number_digits);
  ASSERT_NE(nullptr, decimal_constant);
  char previous_uuid[sql::UuidCommon::LENGTH_UUID] = {};
  ASSERT_EQ(common::OB_SUCCESS, sql::ObExprUuid::gen_server_uuid(previous_uuid, sizeof(previous_uuid)));
  for (int cycle = 0; cycle < 3; ++cycle) {
    ASSERT_EQ(common::OB_SUCCESS, sql::init_sql_expr_static_var());
    EXPECT_EQ(number_digits, common::ObNumberConstValue::MYSQL_MAX[10][2].get_digits());
    EXPECT_EQ(decimal_constant, wide::ObDecimalIntConstValue::get_max_value(10));
    char current_uuid[sql::UuidCommon::LENGTH_UUID] = {};
    ASSERT_EQ(common::OB_SUCCESS, sql::ObExprUuid::init());
    ASSERT_EQ(common::OB_SUCCESS, sql::ObExprUuid::gen_server_uuid(current_uuid, sizeof(current_uuid)));
    EXPECT_NE(std::string(previous_uuid, sizeof(previous_uuid)), std::string(current_uuid, sizeof(current_uuid)));
    // The clock advances while the host node at the end remains the same.
    EXPECT_EQ(std::string(previous_uuid + 24, 12), std::string(current_uuid + 24, 12));
    memcpy(previous_uuid, current_uuid, sizeof(previous_uuid));
  }
}

TEST(DicLoaderLifecycle, DatabaseCloseDropsMapOwnershipAndAllowsNewLoader)
{
  storage::ObGenDicLoader &manager = storage::ObGenDicLoader::get_instance();
  storage::ObDicLoaderHandle old_handle;
  storage::ObDicLoaderHandle new_handle;
  ASSERT_EQ(common::OB_SUCCESS, manager.init());
  ASSERT_EQ(common::OB_SUCCESS, manager.get_dic_loader(
      common::ObString::make_string("ik"), common::CHARSET_UTF8MB4, old_handle));
  ASSERT_TRUE(old_handle.is_valid());
  storage::ObDicLoader *old_loader = old_handle.get_loader();
  ASSERT_GT(old_loader->get_dic_tables_info().count(), 0);
  manager.destroy();
  manager.destroy();
  // Existing handles remain memory-safe until they are released; consumers
  // must not use a handle from a closed database to load a new database.
  EXPECT_EQ(old_loader, old_handle.get_loader());
  EXPECT_GT(old_handle.get_loader()->get_dic_tables_info().count(), 0);
  ASSERT_EQ(common::OB_SUCCESS, manager.init());
  ASSERT_EQ(common::OB_SUCCESS, manager.get_dic_loader(
      common::ObString::make_string("ik"), common::CHARSET_UTF8MB4, new_handle));
  EXPECT_NE(old_loader, new_handle.get_loader());
  old_handle.reset();
  new_handle.reset();
  manager.destroy();
}

TEST(SchemaLifecycle, ClosePreservesBorrowedManagerAndCanBeRetried)
{
  share::schema::ObSchemaMemMgr memory;
  share::schema::ObSchemaMgrCache cache;
  ASSERT_EQ(common::OB_SUCCESS, memory.init("TestSchema"));
  ASSERT_EQ(common::OB_SUCCESS, cache.init(4));
  share::schema::ObSchemaMgr *manager = nullptr;
  ASSERT_EQ(common::OB_SUCCESS, memory.alloc_schema_mgr(manager));
  ASSERT_EQ(common::OB_SUCCESS, manager->init());
  manager->set_schema_version(42);
  share::schema::ObSchemaMgr *eliminated = nullptr;
  share::schema::ObSchemaMgrHandle handle;
  ASSERT_EQ(common::OB_SUCCESS, cache.put(manager, eliminated, &handle));
  ASSERT_EQ(nullptr, eliminated);
  EXPECT_EQ(common::OB_EAGAIN, cache.destroy());
  EXPECT_EQ(42, manager->get_schema_version());
  handle.reset();
  EXPECT_EQ(common::OB_SUCCESS, cache.destroy());
  EXPECT_EQ(common::OB_SUCCESS, cache.destroy());
  memory.destroy();
  memory.destroy();
  ASSERT_EQ(common::OB_SUCCESS, memory.init("TestSchema"));
  ASSERT_EQ(common::OB_SUCCESS, cache.init(4));
  common::ObArray<void *> managers;
  ASSERT_EQ(common::OB_SUCCESS, memory.get_all_ptrs(managers));
  EXPECT_EQ(0, managers.count());
  EXPECT_EQ(common::OB_SUCCESS, cache.destroy());
}

namespace
{
class QueueTestProcessor
{
public:
  int batch_process_tasks(const common::ObIArray<observer::ObServerSchemaTask> &, bool &)
  {
    return common::OB_SUCCESS;
  }
  int process_barrier(observer::ObServerSchemaTask &, bool &)
  {
    return common::OB_SUCCESS;
  }
};
}

TEST(QueueLifecycle, PendingTasksAreReleasedAcrossReinitialization)
{
  QueueTestProcessor processor;
  observer::ObUniqTaskQueue<observer::ObServerSchemaTask, QueueTestProcessor> queue;
  const observer::ObServerSchemaTask task(observer::ObServerSchemaTask::RELEASE);
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, queue.init_only(&processor, 0, 16, "QueueTest"));
  for (int cycle = 0; cycle < 3; ++cycle) {
    ASSERT_EQ(common::OB_SUCCESS, queue.init_only(&processor, 1, 16, "QueueTest"));
    ASSERT_EQ(common::OB_SUCCESS, queue.add(task));
    EXPECT_EQ(1, queue.task_count());
    EXPECT_EQ(common::OB_INIT_TWICE, queue.init_only(&processor, 1, 16, "QueueTest"));
    EXPECT_EQ(1, queue.task_count());
    queue.stop();
    EXPECT_EQ(common::OB_CANCELED, queue.add(task));
    queue.destroy();
    queue.destroy();
    EXPECT_EQ(0, queue.task_count());
    EXPECT_EQ(common::OB_NOT_INIT, queue.add(task));
  }
}


TEST(ConnResourceLifecycle, BorrowedResourcesSurviveCloseAndPermitRetry)
{
  // This standalone service case owns the timer service for its duration.
  struct TimerServiceCleanup {
    ~TimerServiceCleanup() { common::ObTimerService::get_instance().destroy(); }
  } timer_service_cleanup;
  auto &schema = share::schema::ObMultiVersionSchemaService::get_instance();
  common::ObTimer timer;
  sql::ObConnectResourceMgr manager;
  EXPECT_EQ(common::OB_SUCCESS, manager.destroy());
  // Failure after map initialization must undo the schema/timer binding.
  EXPECT_EQ(common::OB_NOT_INIT, manager.init(schema, timer));
  ASSERT_EQ(common::OB_SUCCESS, timer.init("ConnResourceTest"));
  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE(cycle);
    ASSERT_EQ(common::OB_SUCCESS, manager.init(schema, timer));
    EXPECT_EQ(common::OB_INIT_TWICE, manager.init(schema, timer));
    sql::ObConnectResource *first = nullptr;
    sql::ObConnectResource *second = nullptr;
    ASSERT_EQ(common::OB_SUCCESS, manager.get_or_insert_user_resource(123, 10, 10, first));
    ASSERT_NE(nullptr, first);
    EXPECT_EQ(0, first->cur_connections_);
    EXPECT_EQ(0, first->history_connections_);
    EXPECT_EQ(common::OB_SUCCESS, manager.get_or_insert_user_resource(123, 10, 10, second));
    EXPECT_EQ(first, second);
    bool increased = false;
    EXPECT_EQ(common::OB_SUCCESS, manager.increase_user_connections_count(
        10, 10, common::ObString::make_string("test"), first, increased));
    EXPECT_TRUE(increased);
    EXPECT_EQ(common::OB_EAGAIN, manager.destroy());
    EXPECT_EQ(1, first->cur_connections_);
    sql::ObConnectResource *unavailable = nullptr;
    EXPECT_EQ(common::OB_CANCELED,
        manager.get_or_insert_user_resource(456, 10, 10, unavailable));
    EXPECT_EQ(nullptr, unavailable);
    EXPECT_EQ(common::OB_INIT_TWICE, manager.init(schema, timer));
    manager.revert_user_resource(first);
    EXPECT_EQ(nullptr, first);
    manager.revert_user_resource(first); // Null returns do not drop another borrow.
    EXPECT_EQ(common::OB_EAGAIN, manager.destroy());
    if (nullptr != second) {
      EXPECT_EQ(1, second->history_connections_);
    }
    manager.revert_user_resource(second);
    EXPECT_EQ(nullptr, second);
    EXPECT_EQ(common::OB_SUCCESS, manager.destroy());
    EXPECT_EQ(common::OB_SUCCESS, manager.destroy());
  }
}
