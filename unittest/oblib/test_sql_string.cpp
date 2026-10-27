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
#include "lib/string/ob_sql_identifier.h"

using namespace oceanbase::common;

TEST(TestSqlString, quoted_identifier)
{
  const char *inputs[] = {"column", "select", "a`b", "`a", "a`", "``", "a.b", "a'\\b"};
  const char *outputs[] = {"`column`", "`select`", "`a``b`", "```a`", "`a```", "``````", "`a.b`", "`a'\\b`"};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, append_quoted_identifier(sql, ObString::make_string(inputs[i])));
    EXPECT_STREQ(outputs[i], sql.ptr()) << inputs[i];
  }
}

TEST(TestSqlString, append_identifier_to_existing_sql)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("SELECT "));
  ASSERT_EQ(OB_SUCCESS, append_quoted_identifier(sql, ObString::make_string("a`b")));
  ASSERT_EQ(OB_SUCCESS, sql.append(" FROM "));
  ASSERT_EQ(OB_SUCCESS, append_quoted_identifier(sql, ObString::make_string("t")));
  EXPECT_STREQ("SELECT `a``b` FROM `t`", sql.ptr());
}

TEST(TestSqlString, empty_identifier)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, append_quoted_identifier(sql, ObString()));
  EXPECT_STREQ("``", sql.ptr());
}

TEST(TestSqlString, invalid_identifier_does_not_append)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("SELECT "));
  EXPECT_EQ(OB_INVALID_ARGUMENT, append_quoted_identifier(sql, ObString(-1, "x")));
  EXPECT_STREQ("SELECT ", sql.ptr());
}

TEST(TestSqlString, length_delimited_identifier)
{
  const char name[] = {'a', '`', 'b', 'x'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, append_quoted_identifier(sql, ObString(3, name)));
  EXPECT_STREQ("`a``b`", sql.ptr());
}

TEST(TestSqlString, utf8_identifier)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, append_quoted_identifier(sql, ObString::make_string("列`名")));
  EXPECT_STREQ("`列``名`", sql.ptr());
}

TEST(TestSqlString, long_identifier)
{
  const std::string name(1024, '`');
  const std::string expected(2050, '`');
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, append_quoted_identifier(sql, ObString(name.size(), name.data())));
  EXPECT_EQ(expected, std::string(sql.ptr(), sql.length()));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
