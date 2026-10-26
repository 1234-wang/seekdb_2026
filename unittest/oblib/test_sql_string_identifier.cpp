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

#include <string>
#include <gtest/gtest.h>
#include "lib/string/ob_sql_string.h"

using namespace oceanbase::common;

TEST(TestSqlStringIdentifier, mysql_quotes)
{
  const char *inputs[] = {"plain", "select", "a`b", "`edge`", "a``b", "`"};
  const char *expected[] = {"plain", "select", "a``b", "``edge``", "a````b", "``"};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string(inputs[i])));
    EXPECT_STREQ(expected[i], sql.ptr());
  }
}

TEST(TestSqlStringIdentifier, quote_selection)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string("a\"b`c"), '"'));
  EXPECT_STREQ("a\"\"b`c", sql.ptr());
  sql.reuse();
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string("a\"b`c")));
  EXPECT_STREQ("a\"b``c", sql.ptr());
}

TEST(TestSqlStringIdentifier, explicit_length_and_utf8)
{
  const char bytes[] = {'a', '`', 'b', 'X'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(3, bytes)));
  EXPECT_STREQ("a``b", sql.ptr());
  sql.reuse();
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(
      ObString::make_string("\xe5\x88\x97`\xe5\x90\x8d")));
  EXPECT_STREQ("\xe5\x88\x97``\xe5\x90\x8d", sql.ptr());
}

TEST(TestSqlStringIdentifier, ddl_wrappers_and_shadow_prefix)
{
  const ObString name = ObString::make_string("a`b");
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("cast(`"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(name));
  ASSERT_EQ(OB_SUCCESS, sql.append("` as unsigned) AS `__SHADOW_"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(name));
  ASSERT_EQ(OB_SUCCESS, sql.append("`"));
  EXPECT_STREQ("cast(`a``b` as unsigned) AS `__SHADOW_a``b`", sql.ptr());
}

TEST(TestSqlStringIdentifier, growth_and_append)
{
  std::string input;
  std::string expected("prefix:");
  for (int i = 0; i < 600; ++i) {
    input += "x`";
    expected += "x``";
  }
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("prefix:"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(input.size(), input.data())));
  EXPECT_EQ(expected, std::string(sql.ptr(), sql.length()));
}

TEST(TestSqlStringIdentifier, empty_and_invalid_quote)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("prefix"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString()));
  EXPECT_STREQ("prefix", sql.ptr());
  EXPECT_EQ(OB_INVALID_ARGUMENT,
            sql.append_escaped_identifier(ObString::make_string("a'b"), '\''));
  EXPECT_STREQ("prefix", sql.ptr());
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
