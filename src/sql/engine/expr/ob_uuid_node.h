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

#include <stddef.h>

namespace oceanbase
{
namespace sql
{

constexpr int UUID_NODE_LEN = 6;
constexpr int UUID_IF_NAME_LEN = 16;
// Bound interrupted open/read retries so a stuck signal cannot hang startup.
constexpr int UUID_NODE_IO_RETRY_LIMIT = 128;

// open/read/close are non-variadic so tests can replace them. read returns
// the byte count, 0 on EOF, or -1 with errno set.
struct UuidCsprngOps {
  int (*open_fn)(const char *path, int flags);
  int (*read_fn)(int fd, void *buf, size_t count);
  int (*close_fn)(int fd);
};

struct UuidMacOps {
  int (*socket_fn)(int domain, int type, int protocol);
  int (*ioctl_fn)(int fd, unsigned long request, void *arg);
  int (*close_fn)(int fd);
};

struct UuidMacDiscovery {
  bool found;
  int discovery_errno;
  int hw_errno;
  char failed_ifname[UUID_IF_NAME_LEN];
  unsigned char mac[UUID_NODE_LEN];
};

// True when the 6 bytes can identify a host. An all-zero address is what
// tunnel and dummy interfaces report and is not a usable node id.
bool uuid_node_mac_usable(const unsigned char *mac);

// Read UUID_NODE_LEN bytes from an OS CSPRNG and set the RFC 9562 §6.10
// multicast bit. ops == nullptr uses /dev/urandom and caches the result for
// the process. On failure node is left unchanged. Returns 0 or an errno.
int fill_random_uuid_node(unsigned char *node, const UuidCsprngOps *ops);

// Copy a usable MAC, otherwise fill a random node. On failure out is unchanged.
int finish_uuid_node(bool mac_found,
                     const unsigned char *mac,
                     unsigned char *out,
                     bool *used_random,
                     const UuidCsprngOps *ops);

// Linux ioctl discovery. mac_ops == nullptr uses the real socket/ioctl calls.
// Returns 0 when discovery finished (found may still be false). ENOTSUP when
// this platform does not use the ioctl path.
int discover_linux_mac(UuidMacDiscovery *out, const UuidMacOps *mac_ops);

// Prefer a usable MAC. On discovery failure or no usable MAC, use the CSPRNG
// node. Entropy failure returns an errno and leaves out unchanged.
int acquire_linux_uuid_node(unsigned char *out,
                            bool *used_random,
                            UuidMacDiscovery *discovery,
                            const UuidMacOps *mac_ops,
                            const UuidCsprngOps *rng_ops);

#ifdef OB_UUID_NODE_TEST
void uuid_node_test_reset_cache();
#endif

} // namespace sql
} // namespace oceanbase
