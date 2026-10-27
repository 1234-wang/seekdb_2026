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
#include "lib/string/ob_sql_string.h"

using namespace oceanbase::common;

TEST(TestSqlIdentifier, doubles_delimiters)
{
  const char *inputs[] = {"plain", "select", "a`b", "`first", "last`", "a``b", "`"};
  const char *outputs[] = {"plain", "select", "a``b", "``first", "last``", "a````b", "``"};
  for (size_t i = 0; i < ARRAYSIZEOF(inputs); ++i) {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(inputs[i])));
    EXPECT_EQ(ObString(outputs[i]), sql.string());
  }
}

TEST(TestSqlIdentifier, preserves_other_characters)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString("列`名\\\"'")));
  EXPECT_EQ(ObString("列``名\\\"'"), sql.string());
}

TEST(TestSqlIdentifier, supports_double_quoted_identifiers)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString("a\"b`c"), '"'));
  EXPECT_EQ(ObString("a\"\"b`c"), sql.string());
}

TEST(TestSqlIdentifier, preserves_existing_prefix_and_empty_input)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("`__SHADOW_"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString()));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString("a`b")));
  ASSERT_EQ(OB_SUCCESS, sql.append("`"));
  EXPECT_EQ(ObString("`__SHADOW_a``b`"), sql.string());
}

TEST(TestSqlIdentifier, respects_explicit_string_length)
{
  const char input[] = {'a', '`', 'b', 'x'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(3, input)));
  EXPECT_EQ(ObString("a``b"), sql.string());
}

TEST(TestSqlIdentifier, respects_embedded_nul)
{
  const char input[] = {'a', '\0', '`', 'b'};
  const char expected[] = {'a', '\0', '`', '`', 'b'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(sizeof(input), input)));
  EXPECT_EQ(ObString(sizeof(expected), expected), sql.string());
}

TEST(TestSqlIdentifier, rejects_nul_delimiter_without_appending)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("prefix"));
  EXPECT_EQ(OB_INVALID_ARGUMENT, sql.append_escaped_identifier(ObString("name"), '\0'));
  EXPECT_EQ(ObString("prefix"), sql.string());
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
