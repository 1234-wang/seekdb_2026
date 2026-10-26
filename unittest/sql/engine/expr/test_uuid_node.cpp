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

#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <net/if.h>
#ifdef UUID_NODE_TEST_ANDROID
#include <ifaddrs.h>
#include <linux/if_packet.h>
#endif
#include <set>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "sql/engine/expr/uuid_node.h"

using oceanbase::sql::UuidNodeDiscovery;
using oceanbase::sql::init_uuid_node;

namespace
{
enum class DiscoveryMode
{
  REAL,
  MAC,
  EMPTY,
  LOOPBACK,
  SOCKET_ERROR,
  ENUMERATION_ERROR,
  FLAGS_ERROR,
  HARDWARE_ERROR,
  SECOND_MAC,
};

#ifdef UUID_NODE_TEST_ANDROID
const char *enumeration_operation = "getifaddrs";
#else
const char *enumeration_operation = "SIOCGIFCONF";
#endif
DiscoveryMode discovery_mode = DiscoveryMode::MAC;
int discovery_error = EPERM;
int random_result = 1;
bool real_random = false;
std::atomic<int> random_calls(0);
const unsigned char hardware_node[6] = {0x02, 0x12, 0x34, 0x56, 0x78, 0x9a};
const unsigned char random_node[6] = {0x80, 0x21, 0x43, 0x65, 0x87, 0xa9};

class TestUuidNode : public ::testing::Test
{
protected:
  void SetUp() override
  {
    discovery_mode = DiscoveryMode::MAC;
    discovery_error = EPERM;
    random_result = 1;
    real_random = false;
    random_calls = 0;
  }

  void expect_fallback(const char *operation, int error)
  {
    unsigned char node[6] = {};
    UuidNodeDiscovery discovery;
    ASSERT_TRUE(init_uuid_node(node, discovery));
    EXPECT_EQ(1, random_calls.load());
    EXPECT_EQ(0x81, node[0]);
    EXPECT_EQ(0, memcmp(node + 1, random_node + 1, sizeof(node) - 1));
    EXPECT_STREQ(operation, discovery.operation_);
    EXPECT_EQ(error, discovery.error_);
  }
};
} // namespace

#ifndef UUID_NODE_TEST_ANDROID
extern "C" int __real_socket(int, int, int);
extern "C" int __real_ioctl(int, unsigned long, ...);


extern "C" int __wrap_socket(int domain, int type, int protocol)
{
  if (DiscoveryMode::SOCKET_ERROR == discovery_mode) {
    errno = discovery_error;
    return -1;
  }
  return __real_socket(domain, type, protocol);
}

extern "C" int __wrap_ioctl(int fd, unsigned long operation, ...)
{
  va_list arguments;
  va_start(arguments, operation);
  void *arg = va_arg(arguments, void *);
  va_end(arguments);
  if (DiscoveryMode::REAL == discovery_mode) {
    return __real_ioctl(fd, operation, arg);
  } else if (SIOCGIFCONF == operation) {
    if (DiscoveryMode::ENUMERATION_ERROR == discovery_mode) {
      errno = discovery_error;
      return -1;
    }
    struct ifconf *config = static_cast<struct ifconf *>(arg);
    if (DiscoveryMode::EMPTY == discovery_mode) {
      config->ifc_len = 0;
    } else {
      const int count = DiscoveryMode::SECOND_MAC == discovery_mode ? 2 : 1;
      config->ifc_len = count * sizeof(struct ifreq);
      strcpy(config->ifc_req[0].ifr_name, "first");
      if (count == 2) {
        strcpy(config->ifc_req[1].ifr_name, "second");
      }
    }
    return 0;
  } else if (SIOCGIFFLAGS == operation) {
    if (DiscoveryMode::FLAGS_ERROR == discovery_mode) {
      errno = discovery_error;
      return -1;
    }
    struct ifreq *request = static_cast<struct ifreq *>(arg);
    request->ifr_flags = DiscoveryMode::LOOPBACK == discovery_mode ? IFF_LOOPBACK : 0;
    return 0;
  } else if (SIOCGIFHWADDR == operation) {
    struct ifreq *request = static_cast<struct ifreq *>(arg);
    if (DiscoveryMode::HARDWARE_ERROR == discovery_mode
        || (DiscoveryMode::SECOND_MAC == discovery_mode && strcmp(request->ifr_name, "first") == 0)) {
      errno = discovery_error;
      return -1;
    }
    memcpy(request->ifr_hwaddr.sa_data, hardware_node, sizeof(hardware_node));
    return 0;
  }
  errno = EINVAL;
  return -1;
}

#else
extern "C" int __real_getifaddrs(struct ifaddrs **);
extern "C" void __real_freeifaddrs(struct ifaddrs *);

extern "C" int __wrap_getifaddrs(struct ifaddrs **result)
{
  if (DiscoveryMode::REAL == discovery_mode) {
    return __real_getifaddrs(result);
  } else if (DiscoveryMode::ENUMERATION_ERROR == discovery_mode) {
    errno = discovery_error;
    return -1;
  } else if (DiscoveryMode::EMPTY == discovery_mode) {
    *result = nullptr;
    return 0;
  }
  static struct ifaddrs interfaces[2];
  static struct sockaddr_ll addresses[2];
  memset(interfaces, 0, sizeof(interfaces));
  memset(addresses, 0, sizeof(addresses));
  const int count = DiscoveryMode::SECOND_MAC == discovery_mode ? 2 : 1;
  for (int i = 0; i < count; ++i) {
    addresses[i].sll_family = AF_PACKET;
    addresses[i].sll_halen = count == 2 && i == 0 ? 0 : 6;
    memcpy(addresses[i].sll_addr, hardware_node, sizeof(hardware_node));
    interfaces[i].ifa_addr = reinterpret_cast<struct sockaddr *>(&addresses[i]);
    interfaces[i].ifa_flags = DiscoveryMode::LOOPBACK == discovery_mode ? IFF_LOOPBACK : 0;
    interfaces[i].ifa_next = i + 1 < count ? &interfaces[i + 1] : nullptr;
  }
  *result = interfaces;
  return 0;
}

extern "C" void __wrap_freeifaddrs(struct ifaddrs *interfaces)
{
  if (DiscoveryMode::REAL == discovery_mode) {
    __real_freeifaddrs(interfaces);
  }
}
#endif

extern "C" int __real_RAND_bytes(unsigned char *, int);

extern "C" int __wrap_RAND_bytes(unsigned char *buffer, int length)
{
  ++random_calls;
  if (real_random) {
    return __real_RAND_bytes(buffer, length);
  }
  EXPECT_EQ(6, length);
  // Also simulate a generator which touches its buffer before reporting a
  // failure: the caller must not publish those partially generated bytes.
  memcpy(buffer, random_node, length);
  return random_result;
}

TEST_F(TestUuidNode, preserves_hardware_node_without_entropy)
{
  unsigned char node[6] = {};
  UuidNodeDiscovery discovery;
  random_result = 0;
  ASSERT_TRUE(init_uuid_node(node, discovery));
  EXPECT_EQ(0, memcmp(node, hardware_node, sizeof(node)));
  EXPECT_EQ(0, random_calls.load());
  EXPECT_EQ(nullptr, discovery.operation_);
  EXPECT_EQ(0, discovery.error_);
}

TEST_F(TestUuidNode, falls_back_when_interfaces_are_empty)
{
  discovery_mode = DiscoveryMode::EMPTY;
  errno = ENOENT; // This stale value must not be reported as a discovery error.
  expect_fallback("no usable MAC address", 0);
}

TEST_F(TestUuidNode, falls_back_when_only_loopback_is_available)
{
  discovery_mode = DiscoveryMode::LOOPBACK;
  expect_fallback("no usable MAC address", 0);
}

#ifndef UUID_NODE_TEST_ANDROID
TEST_F(TestUuidNode, falls_back_when_socket_is_denied)
{
  discovery_mode = DiscoveryMode::SOCKET_ERROR;
  expect_fallback("socket", EPERM);
}

TEST_F(TestUuidNode, falls_back_when_socket_resources_are_exhausted)
{
  discovery_mode = DiscoveryMode::SOCKET_ERROR;
  discovery_error = EMFILE;
  expect_fallback("socket", EMFILE);
}

#endif

TEST_F(TestUuidNode, falls_back_when_enumeration_is_denied)
{
  discovery_mode = DiscoveryMode::ENUMERATION_ERROR;
  expect_fallback(enumeration_operation, EPERM);
}

#ifndef UUID_NODE_TEST_ANDROID
TEST_F(TestUuidNode, falls_back_when_flags_are_denied)
{
  discovery_mode = DiscoveryMode::FLAGS_ERROR;
  expect_fallback("SIOCGIFFLAGS", EPERM);
}

TEST_F(TestUuidNode, falls_back_when_hardware_address_is_denied)
{
  discovery_mode = DiscoveryMode::HARDWARE_ERROR;
  expect_fallback("SIOCGIFHWADDR", EPERM);
}

#endif

TEST_F(TestUuidNode, continues_to_another_eligible_interface)
{
  discovery_mode = DiscoveryMode::SECOND_MAC;
  unsigned char node[6] = {};
  UuidNodeDiscovery discovery;
  ASSERT_TRUE(init_uuid_node(node, discovery));
  EXPECT_EQ(0, memcmp(node, hardware_node, sizeof(node)));
  EXPECT_EQ(0, random_calls.load());
  EXPECT_EQ(nullptr, discovery.operation_);
  EXPECT_EQ(0, discovery.error_);
}

TEST_F(TestUuidNode, entropy_failure_does_not_publish_node)
{
  discovery_mode = DiscoveryMode::ENUMERATION_ERROR;
  for (int result : {0, -1}) {
    random_result = result;
    unsigned char node[6] = {1, 2, 3, 4, 5, 6};
    const unsigned char original[6] = {1, 2, 3, 4, 5, 6};
    UuidNodeDiscovery discovery;
    ASSERT_FALSE(init_uuid_node(node, discovery));
    EXPECT_EQ(0, memcmp(node, original, sizeof(node)));
    EXPECT_STREQ(enumeration_operation, discovery.operation_);
    EXPECT_EQ(EPERM, discovery.error_);
  }
}

TEST_F(TestUuidNode, can_retry_after_entropy_failure)
{
  discovery_mode = DiscoveryMode::EMPTY;
  random_result = 0;
  unsigned char node[6] = {};
  UuidNodeDiscovery discovery;
  ASSERT_FALSE(init_uuid_node(node, discovery));
  random_result = 1;
  ASSERT_TRUE(init_uuid_node(node, discovery));
  EXPECT_EQ(0x81, node[0]);
}

TEST_F(TestUuidNode, native_discovery_smoke)
{
  discovery_mode = DiscoveryMode::REAL;
  real_random = true;
  unsigned char node[6] = {};
  UuidNodeDiscovery discovery;
  EXPECT_TRUE(init_uuid_node(node, discovery));
}

TEST_F(TestUuidNode, concurrent_secure_fallbacks_are_distinct)
{
  discovery_mode = DiscoveryMode::ENUMERATION_ERROR;
  real_random = true;
  constexpr int count = 32;
  std::array<std::array<unsigned char, 6>, count> nodes = {};
  std::array<bool, count> success = {};
  std::vector<std::thread> threads;
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([&, i]() {
      unsigned char node[6] = {};
      UuidNodeDiscovery discovery;
      success[i] = init_uuid_node(node, discovery);
      memcpy(nodes[i].data(), node, sizeof(node));
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  std::set<std::array<unsigned char, 6>> distinct;
  for (int i = 0; i < count; ++i) {
    ASSERT_TRUE(success[i]);
    EXPECT_EQ(1, nodes[i][0] & 1);
    distinct.insert(nodes[i]);
  }
  EXPECT_EQ(count, distinct.size());
}

TEST_F(TestUuidNode, secure_fallback_is_independent_after_fork)
{
  discovery_mode = DiscoveryMode::EMPTY;
  real_random = true;
  unsigned char parent_node[6] = {};
  UuidNodeDiscovery discovery;
  ASSERT_TRUE(init_uuid_node(parent_node, discovery));
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    unsigned char child_node[6] = {};
    UuidNodeDiscovery child_discovery;
    const bool ok = init_uuid_node(child_node, child_discovery);
    _exit(ok && memcmp(parent_node, child_node, sizeof(parent_node)) != 0 ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(child, waitpid(child, &status, 0));
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
