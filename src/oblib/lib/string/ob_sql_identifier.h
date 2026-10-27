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

#pragma once

#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
namespace common
{

// Append one MySQL identifier, doubling backticks inside the delimiters.
inline int append_quoted_identifier(ObSqlString &sql, const ObString &identifier)
{
  int ret = OB_SUCCESS;
  if (identifier.length() < 0 || (identifier.length() > 0 && NULL == identifier.ptr())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.append("`"))) {
  } else {
    int64_t start = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < identifier.length(); ++i) {
      if ('`' == identifier.ptr()[i]) {
        if (OB_FAIL(sql.append(identifier.ptr() + start, i - start + 1))) {
        } else if (OB_FAIL(sql.append("`"))) {
        } else {
          start = i + 1;
        }
      }
    }
    if (OB_SUCC(ret) && start < identifier.length()) {
      ret = sql.append(identifier.ptr() + start, identifier.length() - start);
    }
    if (OB_SUCC(ret)) {
      ret = sql.append("`");
    }
  }
  return ret;
}

} // namespace common
} // namespace oceanbase
