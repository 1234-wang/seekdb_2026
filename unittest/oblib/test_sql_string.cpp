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

TEST(TestSqlString, escaped_identifier)
{
  struct Case {
    const char *name;
    const char *expected;
  };
  const Case cases[] = {
      {"column", "column"},
      {"key`id", "key``id"},
      {"`key", "``key"},
      {"key`", "key``"},
      {"`", "``"},
      {"``", "````"},
      {"a``b`c", "a````b``c"},
      {"列`名", "列``名"},
      {"with space'\"\\", "with space'\"\\"},
      {"%s`%n", "%s``%n"},
      {"", ""},
  };
  for (const Case &test : cases) {
    SCOPED_TRACE(test.name);
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.append("SELECT `"));
    ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string(test.name)));
    ASSERT_EQ(OB_SUCCESS, sql.append("`"));
    const std::string expected = std::string("SELECT `") + test.expected + "`";
    ASSERT_EQ(expected, std::string(sql.ptr(), sql.length()));
    ASSERT_EQ('\0', sql.ptr()[sql.length()]);
  }
}

TEST(TestSqlString, escaped_identifier_respects_length)
{
  // Neither the suffix after the view nor its terminating NUL is part of the name.
  const char name[] = {'k', '`', 'i', 'd', '`', 'x'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(4, name)));
  ASSERT_STREQ("k``id", sql.ptr());
  ASSERT_EQ(5, sql.length());
}

TEST(TestSqlString, escaped_identifier_empty)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("prefix"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString()));
  ASSERT_STREQ("prefix", sql.ptr());
}

TEST(TestSqlString, escaped_identifier_invalid_length)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("prefix"));
  ASSERT_EQ(OB_INVALID_ARGUMENT, sql.append_escaped_identifier(ObString(-1, "name")));
  ASSERT_STREQ("prefix", sql.ptr());
}

TEST(TestSqlString, escaped_identifier_grows_buffer)
{
  // Cross multiple ObSqlString capacity boundaries while doubling delimiters.
  const std::string name(1024, '`');
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("`__SHADOW_"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(name.size(), name.data())));
  ASSERT_EQ(OB_SUCCESS, sql.append("`"));
  const std::string expected = "`__SHADOW_" + std::string(2048, '`') + "`";
  ASSERT_EQ(expected, std::string(sql.ptr(), sql.length()));
  ASSERT_EQ('\0', sql.ptr()[sql.length()]);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
