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

namespace
{
std::string bytes(const ObSqlString &sql)
{
  return sql.empty() ? std::string() : std::string(sql.ptr(), sql.length());
}

void check_escape(const char *input, const char *expected)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string(input)));
  ASSERT_EQ(std::string(expected), bytes(sql));
}
}

TEST(TestSqlIdentifierEscape, ordinary_names)
{
  check_escape("plain_name_123", "plain_name_123");
  check_escape("中文列名", "中文列名");
  check_escape("a\\b\"c'd", "a\\b\"c'd");
}

TEST(TestSqlIdentifierEscape, delimiter_positions)
{
  check_escape("a`b", "a``b");
  check_escape("`head", "``head");
  check_escape("tail`", "tail``");
  check_escape("a``b", "a````b");
  check_escape("`", "``");
  check_escape("``", "````");
}

TEST(TestSqlIdentifierEscape, appends_and_leaves_quotes_to_caller)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("SELECT `"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string("a`b")));
  ASSERT_EQ(OB_SUCCESS, sql.append("`"));
  ASSERT_EQ("SELECT `a``b`", bytes(sql));
}

TEST(TestSqlIdentifierEscape, empty_input)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString()));
  ASSERT_TRUE(sql.empty());
  ASSERT_EQ(OB_SUCCESS, sql.append("prefix"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string("")));
  ASSERT_EQ("prefix", bytes(sql));
}

TEST(TestSqlIdentifierEscape, respects_explicit_length)
{
  char input[] = {'a', '`', 'b', 'x'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(0, 3, input)));
  ASSERT_EQ("a``b", bytes(sql));
  ASSERT_EQ('x', input[3]);
}

TEST(TestSqlIdentifierEscape, preserves_embedded_nul)
{
  char input[] = {'a', '\0', '`', 'b'};
  const char expected[] = {'a', '\0', '`', '`', 'b'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(0, sizeof(input), input)));
  ASSERT_EQ(std::string(expected, sizeof(expected)), bytes(sql));
}

TEST(TestSqlIdentifierEscape, self_reference_survives_reallocation)
{
  const std::string input(400, '`');
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.assign(input.data(), input.size()));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(sql.string()));
  ASSERT_EQ(std::string(1200, '`'), bytes(sql));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
