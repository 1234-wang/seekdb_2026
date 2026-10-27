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
#include "sql/engine/cmd/ob_kill_session_arg.h"
#include "sql/privilege_check/ob_privilege_check.h"
#include "sql/resolver/ddl/ob_analyze_stmt.h"
#include "sql/resolver/ddl/ob_create_package_stmt.h"
#include "sql/resolver/ddl/ob_drop_package_stmt.h"
#include "sql/resolver/ddl/ob_optimize_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share::schema;

static int collect_privileges(const ObStmt &stmt, ObIArray<ObNeedPriv> &privileges)
{
  ObSessionPrivInfo session_priv;
  session_priv.user_id_ = 42;
  return ObPrivilegeCheck::get_stmt_need_privs(session_priv, &stmt, privileges);
}

TEST(StmtPrivileges, file_outputs_require_file_but_variables_do_not)
{
  const ObItemType modes[] = {T_INTO_OUTFILE, T_INTO_DUMPFILE, T_INTO_VARIABLES};
  for (const ObItemType mode : modes) {
    ObSelectStmt stmt;
    ObSelectIntoItem into;
    into.into_type_ = mode;
    stmt.set_select_into(&into);
    ObSEArray<ObNeedPriv, 4> privileges;
    ASSERT_EQ(OB_SUCCESS, collect_privileges(stmt, privileges));
    int file_requirements = 0;
    for (int64_t i = 0; i < privileges.count(); ++i) {
      if (OB_PRIV_FILE == privileges.at(i).priv_set_) {
        EXPECT_EQ(OB_PRIV_USER_LEVEL, privileges.at(i).priv_level_);
        ++file_requirements;
      }
    }
    EXPECT_EQ(mode == T_INTO_VARIABLES ? 0 : 1, file_requirements);
  }
}

TEST(StmtPrivileges, kill_requires_same_user_or_super)
{
  ObKillSessionArg arg;
  arg.user_id_ = 42;
  EXPECT_EQ(OB_SUCCESS, arg.check_auth_for_kill(1, 42));
  EXPECT_EQ(OB_ERR_KILL_DENIED, arg.check_auth_for_kill(1, 43));
  arg.has_user_super_privilege_ = true;
  EXPECT_EQ(OB_SUCCESS, arg.check_auth_for_kill(1, 43));
  EXPECT_EQ(OB_ERR_KILL_DENIED, arg.check_auth_for_kill(1, OB_INVALID_ID));
  arg.user_id_ = OB_INVALID_ID;
  EXPECT_EQ(OB_ERR_KILL_DENIED, arg.check_auth_for_kill(1, 43));
}

TEST(StmtPrivileges, package_ddl_requires_routine_management_grants)
{
  ObCreatePackageStmt create;
  create.get_create_package_arg().db_name_ = ObString::make_string("package_db");
  create.get_create_package_arg().package_info_.set_type(PACKAGE_TYPE);
  ObSEArray<ObNeedPriv, 4> privileges;
  ASSERT_EQ(OB_SUCCESS, collect_privileges(create, privileges));
  ASSERT_EQ(1, privileges.count());
  EXPECT_EQ(OB_PRIV_DB_LEVEL, privileges.at(0).priv_level_);
  EXPECT_EQ(OB_PRIV_CREATE_ROUTINE, privileges.at(0).priv_set_);
  EXPECT_EQ(ObString::make_string("package_db"), privileges.at(0).db_);

  create.get_create_package_arg().is_replace_ = true;
  privileges.reuse();
  ASSERT_EQ(OB_SUCCESS, collect_privileges(create, privileges));
  ASSERT_EQ(1, privileges.count());
  EXPECT_EQ(OB_PRIV_CREATE_ROUTINE | OB_PRIV_ALTER_ROUTINE, privileges.at(0).priv_set_);

  create.get_create_package_arg().is_replace_ = false;
  create.get_create_package_arg().package_info_.set_type(PACKAGE_BODY_TYPE);
  privileges.reuse();
  ASSERT_EQ(OB_SUCCESS, collect_privileges(create, privileges));
  ASSERT_EQ(1, privileges.count());
  EXPECT_EQ(OB_PRIV_CREATE_ROUTINE | OB_PRIV_ALTER_ROUTINE, privileges.at(0).priv_set_);

  ObDropPackageStmt drop;
  drop.get_drop_package_arg().db_name_ = ObString::make_string("package_db");
  privileges.reuse();
  ASSERT_EQ(OB_SUCCESS, collect_privileges(drop, privileges));
  ASSERT_EQ(1, privileges.count());
  EXPECT_EQ(OB_PRIV_ALTER_ROUTINE, privileges.at(0).priv_set_);
}

TEST(StmtPrivileges, analyze_and_optimize_require_privileges_on_each_table)
{
  ObAnalyzeStmt analyze;
  ASSERT_EQ(OB_SUCCESS, analyze.add_table(ObString::make_string("db1"), 1,
                                        ObString::make_string("t1"), 10001, USER_TABLE));
  ASSERT_EQ(OB_SUCCESS, analyze.add_table(ObString::make_string("db2"), 2,
                                        ObString::make_string("t2"), 10002, USER_TABLE));
  ObSEArray<ObNeedPriv, 4> privileges;
  ASSERT_EQ(OB_SUCCESS, collect_privileges(analyze, privileges));
  ASSERT_EQ(2, privileges.count());
  EXPECT_EQ(ObString::make_string("db1"), privileges.at(0).db_);
  EXPECT_EQ(ObString::make_string("t2"), privileges.at(1).table_);
  for (int64_t i = 0; i < privileges.count(); ++i) {
    EXPECT_EQ(OB_PRIV_TABLE_LEVEL, privileges.at(i).priv_level_);
    EXPECT_EQ(OB_PRIV_SELECT | OB_PRIV_INSERT, privileges.at(i).priv_set_);
  }

  ObOptimizeTableStmt optimize;
  obcall::ObTableItem table;
  table.database_name_ = ObString::make_string("db1");
  table.table_name_ = ObString::make_string("t1");
  ASSERT_EQ(OB_SUCCESS, optimize.add_table_item(table));
  privileges.reuse();
  ASSERT_EQ(OB_SUCCESS, collect_privileges(optimize, privileges));
  ASSERT_EQ(1, privileges.count());
  EXPECT_EQ(OB_PRIV_TABLE_LEVEL, privileges.at(0).priv_level_);
  EXPECT_EQ(OB_PRIV_SELECT | OB_PRIV_INSERT, privileges.at(0).priv_set_);
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
