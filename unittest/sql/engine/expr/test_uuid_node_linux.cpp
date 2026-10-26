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

/* Build from the repository root on Linux:
 * g++ -std=c++17 -Wall -Wextra -Werror -Isrc \
 *   unittest/sql/engine/expr/test_uuid_node_linux.cpp \
 *   -Wl,--wrap=socket,--wrap=ioctl,--wrap=close,--wrap=getrandom -o /tmp/test_uuid_node_linux
 */

#include "sql/engine/expr/uuid_node_linux.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <vector>

using oceanbase::sql::initialize_linux_uuid_node;

enum class Discovery { MAC, SOCKET_ERROR, ENUM_ERROR, EMPTY, LOOPBACK, FLAGS_ERROR, MAC_ERROR, FIRST_FLAGS_ERROR };
static Discovery discovery;
static std::vector<int> entropy_reads;
static size_t entropy_calls;
static size_t random_offset;
static int close_calls;
static bool real_entropy;
static const unsigned char physical_mac[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};

static void reset(Discovery mode)
{
  discovery = mode;
  entropy_reads.clear();
  entropy_calls = 0;
  random_offset = 0;
  close_calls = 0;
  real_entropy = false;
}

extern "C" int __wrap_socket(int, int, int)
{
  if (discovery == Discovery::SOCKET_ERROR) {
    errno = EPERM;
    return -1;
  }
  return 42;
}

extern "C" int __wrap_close(int fd)
{
  assert(fd == 42);
  ++close_calls;
  errno = EBADF; // Cleanup must not overwrite the saved discovery failure.
  return 0;
}

extern "C" int __wrap_ioctl(int fd, unsigned long operation, ...)
{
  assert(fd == 42);
  va_list args;
  va_start(args, operation);
  void *argument = va_arg(args, void *);
  va_end(args);
  if (operation == SIOCGIFCONF) {
    if (discovery == Discovery::ENUM_ERROR) {
      errno = EPERM;
      return -1;
    }
    auto *config = static_cast<struct ifconf *>(argument);
    if (discovery == Discovery::EMPTY) {
      config->ifc_len = 0;
    } else {
      assert(config->ifc_len >= static_cast<int>(2 * sizeof(struct ifreq)));
      config->ifc_len = 2 * sizeof(struct ifreq);
      strcpy(config->ifc_req[0].ifr_name, "lo");
      strcpy(config->ifc_req[1].ifr_name, "eth0");
    }
    return 0;
  }
  auto *request = static_cast<struct ifreq *>(argument);
  if (operation == SIOCGIFFLAGS) {
    if (discovery == Discovery::FLAGS_ERROR ||
        (discovery == Discovery::FIRST_FLAGS_ERROR && strcmp(request->ifr_name, "lo") == 0)) {
      errno = EACCES;
      return -1;
    }
    request->ifr_flags = discovery == Discovery::LOOPBACK || strcmp(request->ifr_name, "lo") == 0
                         ? IFF_LOOPBACK : 0;
  } else {
    assert(operation == SIOCGIFHWADDR);
    if (discovery == Discovery::MAC_ERROR) {
      errno = EPERM;
      return -1;
    }
    memcpy(request->ifr_hwaddr.sa_data, physical_mac, sizeof(physical_mac));
  }
  return 0;
}

extern "C" ssize_t __real_getrandom(void *, size_t, unsigned int);
extern "C" ssize_t __wrap_getrandom(void *buffer, size_t length, unsigned int flags)
{
  assert(flags == 0);
  if (real_entropy) {
    return __real_getrandom(buffer, length, flags);
  }
  const int count = entropy_calls < entropy_reads.size()
                    ? entropy_reads[entropy_calls] : static_cast<int>(length);
  ++entropy_calls;
  if (count < 0) {
    errno = -count;
    return -1;
  }
  assert(static_cast<size_t>(count) <= length);
  auto *bytes = static_cast<unsigned char *>(buffer);
  for (int i = 0; i < count; ++i) {
    bytes[i] = static_cast<unsigned char>(0x20 + random_offset++);
  }
  return count;
}

int main()
{
  unsigned char node[6] = {};
  for (const auto mode : {Discovery::MAC, Discovery::FIRST_FLAGS_ERROR}) {
    reset(mode);
    const auto result = initialize_linux_uuid_node(node);
    assert(result.success && !result.random_node);
    assert(memcmp(node, physical_mac, sizeof(node)) == 0);
    assert(entropy_calls == 0 && close_calls == 1);
  }
  for (const auto mode : {Discovery::SOCKET_ERROR, Discovery::ENUM_ERROR, Discovery::EMPTY,
                          Discovery::LOOPBACK, Discovery::FLAGS_ERROR, Discovery::MAC_ERROR}) {
    reset(mode);
    const auto result = initialize_linux_uuid_node(node);
    assert(result.success && result.random_node && result.entropy_errno == 0);
    assert(node[0] == 0x21 && node[5] == 0x25);
    assert(close_calls == (mode == Discovery::SOCKET_ERROR ? 0 : 1));
    const int expected_errno = mode == Discovery::EMPTY || mode == Discovery::LOOPBACK
                               ? 0 : (mode == Discovery::FLAGS_ERROR ? EACCES : EPERM);
    assert(result.discovery_errno == expected_errno);
  }
  reset(Discovery::ENUM_ERROR);
  entropy_reads = {-EINTR, 2, -EINTR, 4};
  assert(initialize_linux_uuid_node(node).success);
  assert(entropy_calls == 4 && node[0] == 0x21 && node[5] == 0x25);
  for (const int failure : {-EACCES, 0}) {
    reset(Discovery::ENUM_ERROR);
    entropy_reads = {2, failure};
    memset(node, 0xaa, sizeof(node));
    const auto result = initialize_linux_uuid_node(node);
    assert(!result.success && !result.random_node && result.discovery_errno == EPERM);
    assert(result.entropy_errno == (failure == 0 ? EIO : EACCES));
    for (const auto byte : node) {
      assert(byte == 0xaa);
    }
  }
  reset(Discovery::ENUM_ERROR);
  real_entropy = true;
  unsigned char second[6] = {};
  assert(initialize_linux_uuid_node(node).success);
  assert(initialize_linux_uuid_node(second).success);
  assert((node[0] & 1) != 0 && (second[0] & 1) != 0);
  assert(memcmp(node, second, sizeof(node)) != 0);
  puts("UUID node regression checks passed, including real Linux entropy fallback");
}
