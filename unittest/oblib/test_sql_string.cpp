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

#define USING_LOG_PREFIX LIB

#include <gtest/gtest.h>
#include "lib/string/ob_sql_string.h"

using namespace oceanbase::common;

TEST(TestSqlString, escape_identifier)
{
  const char *inputs[] = {"plain", "select", "a b", "a`b", "`leading", "trailing`", "``", "中文`列"};
  const char *expected[] = {"plain", "select", "a b", "a``b", "``leading", "trailing``", "````", "中文``列"};
  for (int64_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string(inputs[i])));
    ASSERT_STREQ(expected[i], sql.ptr());
  }
}

TEST(TestSqlString, append_identifier_to_existing_sql)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("`__SHADOW_"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string("a`b")));
  ASSERT_EQ(OB_SUCCESS, sql.append("`"));
  ASSERT_STREQ("`__SHADOW_a``b`", sql.ptr());
}

TEST(TestSqlString, empty_identifier)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("prefix"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString()));
  ASSERT_STREQ("prefix", sql.ptr());
}

TEST(TestSqlString, identifier_is_length_delimited)
{
  // The input need not be terminated; bytes past its length must not be read.
  const char input[] = {'a', '`', 'b', '`'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(3, input)));
  ASSERT_EQ(4, sql.length());
  ASSERT_STREQ("a``b", sql.ptr());
}

TEST(TestSqlString, long_identifier_requires_growth)
{
  char input[512];
  MEMSET(input, '`', sizeof(input));
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(sizeof(input), input)));
  ASSERT_EQ(2 * sizeof(input), sql.length());
  for (int64_t i = 0; i < sql.length(); ++i) {
    ASSERT_EQ('`', sql.ptr()[i]);
  }
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
