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
#include "lib/container/ob_array.h"
#include "lib/string/ob_sql_string.h"
#include "share/ob_ddl_common.h"

using namespace oceanbase::common;
using namespace oceanbase::share;

TEST(TestDDLIdentifier, plain_projection)
{
  ObSqlString sql;
  const ObColumnNameInfo column(ObString::make_string("name"), false);
  ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(column, true, true, false, sql));
  EXPECT_STREQ("`name` AS `name`", sql.ptr());
}

TEST(TestDDLIdentifier, escaped_projection)
{
  ObSqlString sql;
  const ObColumnNameInfo column(ObString::make_string("a`b"), false);
  ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(column, true, true, false, sql));
  EXPECT_STREQ("`a``b` AS `a``b`", sql.ptr());
}

TEST(TestDDLIdentifier, escaped_enum_set_cast)
{
  ObSqlString sql;
  const ObColumnNameInfo column(ObString::make_string("a`b"), false, true);
  ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(column, true, true, false, sql));
  EXPECT_STREQ("cast(`a``b` as unsigned) AS `a``b`", sql.ptr());
}

TEST(TestDDLIdentifier, shadow_alias)
{
  ObSqlString sql;
  const ObColumnNameInfo column(ObString::make_string("a`b"), true);
  ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(column, true, true, false, sql));
  EXPECT_STREQ("`a``b` AS `__SHADOW_a``b`", sql.ptr());
}

TEST(TestDDLIdentifier, comma_and_alias_only)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("`first`"));
  const ObColumnNameInfo column(ObString::make_string("a`b"), true);
  ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(column, false, true, true, sql));
  EXPECT_STREQ("`first`, `__SHADOW_a``b`", sql.ptr());
}

TEST(TestDDLIdentifier, insert_column_list)
{
  ObArray<ObColumnNameInfo> columns;
  ASSERT_EQ(OB_SUCCESS, columns.push_back(ObColumnNameInfo(ObString::make_string("a`b"), false)));
  ASSERT_EQ(OB_SUCCESS, columns.push_back(ObColumnNameInfo(ObString::make_string("c`"), false)));
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(columns, true, false, false, sql));
  EXPECT_STREQ("`a``b`, `c```", sql.ptr());
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
