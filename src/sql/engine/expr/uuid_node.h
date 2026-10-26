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

namespace oceanbase
{
namespace sql
{

struct UuidNodeDiscovery
{
  // Null on successful MAC discovery; otherwise the failed operation or the
  // reason no eligible interface was found. error_ is captured at that step.
  const char *operation_ = nullptr;
  int error_ = 0;
};

// Linux/Android UUIDv1 node discovery. A missing MAC is replaced by six secure
// random bytes with the multicast bit set. On entropy failure, leave node
// unchanged and return false. discovery describes MAC discovery separately.
bool init_uuid_node(unsigned char (&node)[6], UuidNodeDiscovery &discovery);

} // namespace sql
} // namespace oceanbase
