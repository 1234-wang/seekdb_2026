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
#include "share/ob_ddl_common.h"

using namespace oceanbase::common;
using namespace oceanbase::share;

TEST(TestDDLColumnName, identifier_escaping)
{
  struct Case {
    const char *name;
    const char *escaped;
  };
  const Case cases[] = {
      {"column", "column"},
      {"key`id", "key``id"},
      {"`a``b`", "``a````b``"},
      {"列`名", "列``名"},
  };
  for (const Case &test : cases) {
    SCOPED_TRACE(test.name);
    // Exercise the original, alias-only and original-AS-alias forms, including
    // enum/set casts and shadow prefixes. Ordinary names retain their formatting.
    for (int flags = 0; flags < 32; ++flags) {
      SCOPED_TRACE(flags);
      const bool origin = 0 != (flags & 1);
      const bool alias = 0 != (flags & 2);
      const bool comma = 0 != (flags & 4);
      const bool shadow = 0 != (flags & 8);
      const bool cast = 0 != (flags & 16);
      const ObColumnNameInfo info(ObString::make_string(test.name), shadow, cast);
      ObSqlString sql;
      ASSERT_EQ(OB_SUCCESS, sql.append("existing"));
      ASSERT_EQ(OB_SUCCESS,
          ObDDLUtil::generate_column_name_str(info, origin, alias, comma, sql));
      std::string expected = "existing";
      if (comma) {
        expected += ", ";
      }
      if (origin) {
        expected += cast ? "cast(`" : "`";
        expected += test.escaped;
        expected += cast ? "` as unsigned)" : "`";
      }
      if (origin && alias) {
        expected += " AS ";
      }
      if (alias) {
        expected += shadow ? "`__SHADOW_" : "`";
        expected += test.escaped;
        expected += "`";
      }
      ASSERT_EQ(expected, std::string(sql.ptr(), sql.length()));
    }
  }
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
