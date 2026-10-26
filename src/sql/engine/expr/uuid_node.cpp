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

#include "sql/engine/expr/uuid_node.h"

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <net/if.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <ifaddrs.h>
#include <linux/if_packet.h>
#else
#include <sys/ioctl.h>
#endif

namespace oceanbase
{
namespace sql
{
namespace
{

bool discover_uuid_mac(unsigned char (&node)[6], UuidNodeDiscovery &discovery)
{
  bool found = false;
  discovery.operation_ = "no usable MAC address";
  discovery.error_ = 0;
#ifdef __ANDROID__
  struct ifaddrs *interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) {
    discovery.operation_ = "getifaddrs";
    discovery.error_ = errno;
  } else {
    for (struct ifaddrs *it = interfaces; it != nullptr && !found; it = it->ifa_next) {
      if (it->ifa_addr != nullptr && it->ifa_addr->sa_family == AF_PACKET
          && (it->ifa_flags & IFF_LOOPBACK) == 0) {
        const struct sockaddr_ll *addr = reinterpret_cast<const struct sockaddr_ll *>(it->ifa_addr);
        if (addr->sll_halen == sizeof(node)) {
          memcpy(node, addr->sll_addr, sizeof(node));
          found = true;
        }
      }
    }
    freeifaddrs(interfaces);
  }
#else
  const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    discovery.operation_ = "socket";
    discovery.error_ = errno;
  } else {
    struct ifreq interfaces[32] = {};
    struct ifconf config = {};
    config.ifc_len = sizeof(interfaces);
    config.ifc_req = interfaces;
    if (ioctl(sock, SIOCGIFCONF, &config) < 0) {
      discovery.operation_ = "SIOCGIFCONF";
      discovery.error_ = errno;
    } else {
      const int count = config.ifc_len / sizeof(struct ifreq);
      for (int i = 0; i < count && !found; ++i) {
        struct ifreq request = {};
        strncpy(request.ifr_name, interfaces[i].ifr_name, IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
          discovery.operation_ = "SIOCGIFFLAGS";
          discovery.error_ = errno;
        } else if ((request.ifr_flags & IFF_LOOPBACK) == 0) {
          if (ioctl(sock, SIOCGIFHWADDR, &request) < 0) {
            discovery.operation_ = "SIOCGIFHWADDR";
            discovery.error_ = errno;
          } else {
            memcpy(node, request.ifr_hwaddr.sa_data, sizeof(node));
            found = true;
          }
        }
      }
    }
    close(sock);
  }
#endif
  if (found) {
    discovery = UuidNodeDiscovery();
  }
  return found;
}

} // namespace

bool init_uuid_node(unsigned char (&node)[6], UuidNodeDiscovery &discovery)
{
  unsigned char candidate[sizeof(node)] = {};
  if (!discover_uuid_mac(candidate, discovery)) {
    // RAND_bytes uses OpenSSL's OS-seeded CSPRNG and handles entropy reads
    // internally. Both 0 (failure) and -1 (unsupported) must remain fatal.
    if (RAND_bytes(candidate, sizeof(candidate)) != 1) {
      return false;
    }
    // RFC 9562 section 6.10: identify a random node as a non-IEEE address.
    candidate[0] |= 0x01;
  }
  memcpy(node, candidate, sizeof(node));
  return true;
}

} // namespace sql
} // namespace oceanbase
#endif // __linux__
