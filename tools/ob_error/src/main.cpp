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

#include "ob_error.h"

int main(int args, char* argv[])
{
  if (!init_global_info()) {
    printf("\nerror: ob_error init failed.\n");
  } else if (1 < args) {
    if (!parse_param(args, argv)) {
      printf("error: parameters invalid\n"
             "Use 'ob_error --help' for help.\n");
    }
  } else {
    printf("error: missing parameter\n"
           "Use 'ob_error --help' for help.\n");
  }

  return 0;
}
