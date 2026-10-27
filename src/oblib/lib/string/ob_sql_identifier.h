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

#pragma once

#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
namespace common
{

// Quote one MySQL identifier, including an optional prefix, without SQL-layer dependencies.
inline int append_mysql_identifier(ObSqlString &sql,
                                   const ObString &name,
                                   const ObString &prefix = ObString())
{
  int ret = OB_SUCCESS;
  const ObString parts[] = {prefix, name};
  if (OB_FAIL(sql.append("`"))) {
  } else {
    for (int64_t part = 0; OB_SUCC(ret) && part < 2; ++part) {
      const ObString &text = parts[part];
      int64_t start = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < text.length(); ++i) {
        if ('`' == text.ptr()[i]) {
          if (OB_FAIL(sql.append(text.ptr() + start, i - start + 1))) {
          } else if (OB_FAIL(sql.append("`"))) {
          }
          start = i + 1;
        }
      }
      if (OB_SUCC(ret) && start < text.length()) {
        ret = sql.append(text.ptr() + start, text.length() - start);
      }
    }
    if (OB_SUCC(ret)) {
      ret = sql.append("`");
    }
  }
  return ret;
}

} // namespace common
} // namespace oceanbase
