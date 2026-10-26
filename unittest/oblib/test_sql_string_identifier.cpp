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

#include <cstring>
#include <string>
#include <gtest/gtest.h>
#include "lib/string/ob_sql_string.h"

using namespace oceanbase::common;

TEST(TestSqlStringIdentifier, quote_identifier)
{
  struct TestCase {
    const char *identifier;
    const char *expected;
  };
  const TestCase cases[] = {
      {"plain", "`plain`"},
      {"select", "`select`"},
      {"a`b", "`a``b`"},
      {"`a`", "```a```"},
      {"``", "``````"},
      {"", "``"},
      {"a'b\\c", "`a'b\\c`"},
      {"列`名", "`列``名`"},
  };
  for (const TestCase &test : cases) {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.append_quoted_identifier(ObString::make_string(test.identifier)));
    EXPECT_STREQ(test.expected, sql.ptr());
    EXPECT_EQ(strlen(test.expected), sql.length());
  }
}

TEST(TestSqlStringIdentifier, shadow_alias_and_enum_cast)
{
  const ObString name = ObString::make_string("e`num");
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("cast("));
  ASSERT_EQ(OB_SUCCESS, sql.append_quoted_identifier(name));
  ASSERT_EQ(OB_SUCCESS, sql.append(" as unsigned) AS "));
  ASSERT_EQ(OB_SUCCESS, sql.append_quoted_identifier(name, ObString::make_string("__SHADOW_")));
  EXPECT_STREQ("cast(`e``num` as unsigned) AS `__SHADOW_e``num`", sql.ptr());
}

TEST(TestSqlStringIdentifier, escape_prefix)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_quoted_identifier(
      ObString::make_string("`name"), ObString::make_string("prefix`")));
  EXPECT_STREQ("`prefix````name`", sql.ptr());
}

TEST(TestSqlStringIdentifier, length_delimited_identifier)
{
  char name[] = {'a', '`', 'b', 'x'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_quoted_identifier(ObString(3, name)));
  EXPECT_STREQ("`a``b`", sql.ptr());

  sql.reuse();
  ASSERT_EQ(OB_SUCCESS, sql.append_quoted_identifier(ObString()));
  EXPECT_STREQ("``", sql.ptr());
}

TEST(TestSqlStringIdentifier, grow_buffer)
{
  const std::string name(1024, '`');
  const std::string expected = "`" + std::string(2048, '`') + "`";
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("SELECT "));
  ASSERT_EQ(OB_SUCCESS, sql.append_quoted_identifier(ObString(name.size(), name.data())));
  ASSERT_EQ(OB_SUCCESS, sql.append(", "));
  ASSERT_EQ(OB_SUCCESS, sql.append_quoted_identifier(ObString::make_string("tail")));
  EXPECT_EQ("SELECT " + expected + ", `tail`", std::string(sql.ptr(), sql.length()));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
