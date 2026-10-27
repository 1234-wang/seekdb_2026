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
#include "lib/string/ob_sql_identifier.h"

using oceanbase::common::ObSqlString;
using oceanbase::common::ObString;
using oceanbase::common::OB_SUCCESS;
using oceanbase::common::append_mysql_identifier;

TEST(DDLIdentifier, escapes_delimiters_without_changing_other_bytes)
{
  const char *names[] = {"plain", "select", "space name", "`", "a`b", "``", "quote\"name", "反引号`列"};
  const char *quoted[] = {"`plain`", "`select`", "`space name`", "````", "`a``b`", "``````", "`quote\"name`", "`反引号``列`"};
  for (int64_t i = 0; i < 8; ++i) {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, append_mysql_identifier(sql, ObString(names[i])));
    EXPECT_STREQ(quoted[i], sql.ptr());
  }
}

TEST(DDLIdentifier, preserves_existing_sql_and_quotes_shadow_alias)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("cast("));
  ASSERT_EQ(OB_SUCCESS, append_mysql_identifier(sql, ObString("e`num")));
  ASSERT_EQ(OB_SUCCESS, sql.append(" as unsigned) AS "));
  ASSERT_EQ(OB_SUCCESS, append_mysql_identifier(sql, ObString("e`num"), ObString("__SHADOW_")));
  EXPECT_STREQ("cast(`e``num` as unsigned) AS `__SHADOW_e``num`", sql.ptr());
}

TEST(DDLIdentifier, respects_explicit_length_and_empty_components)
{
  ObSqlString sql;
  const char name[] = {'a', '`', 'b', 'x'};
  ASSERT_EQ(OB_SUCCESS, append_mysql_identifier(sql, ObString(3, name)));
  EXPECT_STREQ("`a``b`", sql.ptr());
  sql.reuse();
  ASSERT_EQ(OB_SUCCESS, append_mysql_identifier(sql, ObString()));
  EXPECT_STREQ("``", sql.ptr());
  sql.reuse();
  ASSERT_EQ(OB_SUCCESS, append_mysql_identifier(sql, ObString("name"), ObString("p`")));
  EXPECT_STREQ("`p``name`", sql.ptr());
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
