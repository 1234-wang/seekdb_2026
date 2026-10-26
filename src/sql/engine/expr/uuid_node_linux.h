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

#include <cerrno>
#include <cstring>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

namespace oceanbase
{
namespace sql
{

struct LinuxUUIDNodeResult
{
  bool success = false;
  bool random_node = false;
  const char *discovery_operation = "no non-loopback MAC address";
  int discovery_errno = 0;
  int entropy_errno = 0;
};

// Keep all bytes private until discovery or a complete entropy read succeeds.
inline LinuxUUIDNodeResult initialize_linux_uuid_node(unsigned char (&node)[6])
{
  LinuxUUIDNodeResult result;
  unsigned char candidate[6] = {};
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    result.discovery_errno = errno;
    result.discovery_operation = "socket";
  } else {
    struct ifreq interfaces[32] = {};
    struct ifconf config = {};
    config.ifc_len = sizeof(interfaces);
    config.ifc_req = interfaces;
    if (ioctl(sock, SIOCGIFCONF, &config) < 0) {
      result.discovery_errno = errno;
      result.discovery_operation = "SIOCGIFCONF";
    } else {
      const size_t count = static_cast<size_t>(config.ifc_len) / sizeof(struct ifreq);
      for (size_t i = 0; i < count && !result.success; ++i) {
        struct ifreq request = {};
        memcpy(request.ifr_name, interfaces[i].ifr_name, IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
          result.discovery_errno = errno;
          result.discovery_operation = "SIOCGIFFLAGS";
        } else if (!(request.ifr_flags & IFF_LOOPBACK)) {
          if (ioctl(sock, SIOCGIFHWADDR, &request) < 0) {
            result.discovery_errno = errno;
            result.discovery_operation = "SIOCGIFHWADDR";
          } else {
            memcpy(candidate, request.ifr_hwaddr.sa_data, sizeof(candidate));
            result.success = true;
          }
        }
      }
    }
    close(sock);
  }
  if (!result.success) {
    size_t offset = 0;
    while (offset < sizeof(candidate)) {
      const ssize_t bytes = getrandom(candidate + offset, sizeof(candidate) - offset, 0);
      if (bytes > 0) {
        offset += static_cast<size_t>(bytes);
      } else if (bytes < 0 && errno == EINTR) {
        continue;
      } else {
        result.entropy_errno = bytes == 0 ? EIO : errno;
        return result;
      }
    }
    // RFC 9562 section 6.10: synthetic nodes must set the multicast bit.
    candidate[0] |= 0x01;
    result.random_node = true;
    result.success = true;
  }
  memcpy(node, candidate, sizeof(candidate));
  return result;
}

} // namespace sql
} // namespace oceanbase
