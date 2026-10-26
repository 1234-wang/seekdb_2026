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

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sql/engine/expr/ob_uuid_node.h"

extern char **environ;

namespace oceanbase
{
namespace sql
{
namespace
{

struct FakeNic {
  const char *name;
  short flags;
  unsigned char mac[UUID_NODE_LEN];
  int flags_errno;
  int hw_errno;
};

struct FakeNet {
  int socket_errno;
  int conf_errno;
  int nic_count;
  int socket_calls;
  int close_calls;
  int ioctl_calls;
  int hw_calls;
  FakeNic nics[8];
};

struct FakeRng {
  unsigned char data[UUID_NODE_LEN];
  int open_eintr_left;
  int open_errno;
  bool always_open_eintr;
  int read_eintr_left;
  bool always_read_eintr;
  int read_errno;
  int chunk;
  int offset;
  int eof_after;
  int open_calls;
  int read_calls;
  int close_calls;
  bool fail_if_called;
  int last_flags;
};

FakeNet *g_net = nullptr;
FakeRng *g_rng = nullptr;

void fill_pattern(unsigned char *buf, unsigned char value)
{
  memset(buf, value, UUID_NODE_LEN);
}

bool same_bytes(const unsigned char *buf, unsigned char value)
{
  for (int i = 0; i < UUID_NODE_LEN; ++i) {
    if (buf[i] != value) {
      return false;
    }
  }
  return true;
}

int fake_socket(int domain, int type, int protocol)
{
  (void)domain;
  (void)type;
  (void)protocol;
  ++g_net->socket_calls;
  if (g_net->socket_errno != 0) {
    errno = g_net->socket_errno;
    return -1;
  }
  return 8;
}

int fake_net_close(int fd)
{
  (void)fd;
  ++g_net->close_calls;
  return 0;
}

int fake_ioctl(int fd, unsigned long request, void *arg)
{
  (void)fd;
  ++g_net->ioctl_calls;
  if (request == SIOCGIFCONF) {
    if (g_net->conf_errno != 0) {
      errno = g_net->conf_errno;
      return -1;
    }
    struct ifconf *ifc = static_cast<struct ifconf *>(arg);
    struct ifreq *out = ifc->ifc_req;
    int written = 0;
    for (int i = 0; i < g_net->nic_count; ++i) {
      if ((written + 1) * static_cast<int>(sizeof(struct ifreq)) > ifc->ifc_len) {
        break;
      }
      memset(&out[written], 0, sizeof(out[written]));
      strncpy(out[written].ifr_name, g_net->nics[i].name, IFNAMSIZ - 1);
      ++written;
    }
    ifc->ifc_len = written * static_cast<int>(sizeof(struct ifreq));
    return 0;
  }

  struct ifreq *ifr = static_cast<struct ifreq *>(arg);
  FakeNic *nic = nullptr;
  for (int i = 0; i < g_net->nic_count; ++i) {
    if (strncmp(ifr->ifr_name, g_net->nics[i].name, IFNAMSIZ) == 0) {
      nic = &g_net->nics[i];
      break;
    }
  }
  if (nic == nullptr) {
    errno = ENODEV;
    return -1;
  }
  if (request == SIOCGIFFLAGS) {
    if (nic->flags_errno != 0) {
      errno = nic->flags_errno;
      return -1;
    }
    ifr->ifr_flags = nic->flags;
    return 0;
  }
  if (request == SIOCGIFHWADDR) {
    ++g_net->hw_calls;
    if (nic->hw_errno != 0) {
      errno = nic->hw_errno;
      return -1;
    }
    memcpy(ifr->ifr_hwaddr.sa_data, nic->mac, UUID_NODE_LEN);
    return 0;
  }
  errno = EINVAL;
  return -1;
}

UuidMacOps net_ops()
{
  UuidMacOps ops;
  ops.socket_fn = fake_socket;
  ops.ioctl_fn = fake_ioctl;
  ops.close_fn = fake_net_close;
  return ops;
}

int fake_open(const char *path, int flags)
{
  ++g_rng->open_calls;
  g_rng->last_flags = flags;
  if (path == nullptr || strcmp(path, "/dev/urandom") != 0) {
    errno = ENOENT;
    return -1;
  }
  if ((flags & O_CLOEXEC) == 0 || (flags & O_ACCMODE) != O_RDONLY) {
    errno = EINVAL;
    return -1;
  }
  if (g_rng->always_open_eintr || g_rng->open_eintr_left > 0) {
    if (g_rng->open_eintr_left > 0) {
      --g_rng->open_eintr_left;
    }
    errno = EINTR;
    return -1;
  }
  if (g_rng->open_errno != 0) {
    errno = g_rng->open_errno;
    return -1;
  }
  return 11;
}

int fake_read(int fd, void *buf, size_t count)
{
  (void)fd;
  ++g_rng->read_calls;
  if (g_rng->fail_if_called) {
    errno = EIO;
    return -1;
  }
  if (g_rng->always_read_eintr || g_rng->read_eintr_left > 0) {
    if (g_rng->read_eintr_left > 0) {
      --g_rng->read_eintr_left;
    }
    errno = EINTR;
    return -1;
  }
  if (g_rng->read_errno != 0) {
    errno = g_rng->read_errno;
    return -1;
  }
  if (g_rng->eof_after > 0 && g_rng->offset >= g_rng->eof_after) {
    return 0;
  }
  if (g_rng->chunk <= 0 || g_rng->offset >= UUID_NODE_LEN) {
    return 0;
  }
  int n = g_rng->chunk;
  if (n > static_cast<int>(count)) {
    n = static_cast<int>(count);
  }
  if (g_rng->offset + n > UUID_NODE_LEN) {
    n = UUID_NODE_LEN - g_rng->offset;
  }
  memcpy(buf, g_rng->data + g_rng->offset, static_cast<size_t>(n));
  g_rng->offset += n;
  return n;
}

int fake_rng_close(int fd)
{
  (void)fd;
  ++g_rng->close_calls;
  return 0;
}

UuidCsprngOps rng_ops()
{
  UuidCsprngOps ops;
  ops.open_fn = fake_open;
  ops.read_fn = fake_read;
  ops.close_fn = fake_rng_close;
  return ops;
}

void set_nic(FakeNic *nic, const char *name, short flags, const unsigned char mac[UUID_NODE_LEN])
{
  memset(nic, 0, sizeof(*nic));
  nic->name = name;
  nic->flags = flags;
  memcpy(nic->mac, mac, UUID_NODE_LEN);
}

void expect_multicast(const unsigned char *node)
{
  EXPECT_NE(0, node[0] & 0x01);
}

} // namespace

TEST(UuidNode, null_arguments_do_not_write)
{
  unsigned char node[UUID_NODE_LEN];
  fill_pattern(node, 0xA5);
  EXPECT_EQ(EINVAL, fill_random_uuid_node(nullptr, nullptr));
  bool used_random = false;
  EXPECT_EQ(EINVAL, finish_uuid_node(false, nullptr, nullptr, &used_random, nullptr));
  EXPECT_EQ(EINVAL, finish_uuid_node(false, nullptr, node, nullptr, nullptr));
  EXPECT_TRUE(same_bytes(node, 0xA5));
  UuidMacDiscovery discovery;
  EXPECT_EQ(EINVAL, discover_linux_mac(nullptr, nullptr));
  EXPECT_EQ(EINVAL, acquire_linux_uuid_node(nullptr, &used_random, &discovery, nullptr, nullptr));
  EXPECT_TRUE(same_bytes(node, 0xA5));
}

TEST(UuidNode, zero_mac_is_not_usable)
{
  const unsigned char zeros[UUID_NODE_LEN] = {};
  unsigned char mac[UUID_NODE_LEN] = {0, 0, 0, 0, 0, 1};
  EXPECT_FALSE(uuid_node_mac_usable(nullptr));
  EXPECT_FALSE(uuid_node_mac_usable(zeros));
  EXPECT_TRUE(uuid_node_mac_usable(mac));
}

TEST(UuidNode, short_reads_and_eintr_set_multicast_bit)
{
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  unsigned char raw[UUID_NODE_LEN] = {0x10, 0x22, 0x33, 0x44, 0x55, 0x66};
  memcpy(rng.data, raw, UUID_NODE_LEN);
  rng.open_eintr_left = 2;
  rng.read_eintr_left = 3;
  rng.chunk = 1;
  g_rng = &rng;
  const UuidCsprngOps ops = rng_ops();
  unsigned char node[UUID_NODE_LEN];
  fill_pattern(node, 0xA5);
  ASSERT_EQ(0, fill_random_uuid_node(node, &ops));
  EXPECT_EQ(3, rng.open_calls);
  EXPECT_EQ(UUID_NODE_LEN + 3, rng.read_calls);
  EXPECT_EQ(1, rng.close_calls);
  EXPECT_NE(0, rng.last_flags & O_CLOEXEC);
  EXPECT_EQ(0x11, node[0]);
  EXPECT_EQ(0, memcmp(node + 1, raw + 1, UUID_NODE_LEN - 1));
  expect_multicast(node);
}

TEST(UuidNode, multicast_bit_already_set_stays_set)
{
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  unsigned char raw[UUID_NODE_LEN] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  memcpy(rng.data, raw, UUID_NODE_LEN);
  rng.chunk = UUID_NODE_LEN;
  g_rng = &rng;
  const UuidCsprngOps ops = rng_ops();
  unsigned char node[UUID_NODE_LEN];
  ASSERT_EQ(0, fill_random_uuid_node(node, &ops));
  EXPECT_EQ(0, memcmp(node, raw, UUID_NODE_LEN));
}

TEST(UuidNode, local_admin_bit_is_not_forced)
{
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  rng.data[0] = 0x00;
  rng.data[1] = 0xAB;
  rng.chunk = UUID_NODE_LEN;
  g_rng = &rng;
  const UuidCsprngOps ops = rng_ops();
  unsigned char node[UUID_NODE_LEN];
  ASSERT_EQ(0, fill_random_uuid_node(node, &ops));
  EXPECT_EQ(0x01, node[0]);
  EXPECT_EQ(0xAB, node[1]);
}

TEST(UuidNode, open_failure_leaves_buffer_unchanged)
{
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  rng.open_errno = ENOENT;
  g_rng = &rng;
  const UuidCsprngOps ops = rng_ops();
  unsigned char node[UUID_NODE_LEN];
  fill_pattern(node, 0xA5);
  EXPECT_EQ(ENOENT, fill_random_uuid_node(node, &ops));
  EXPECT_TRUE(same_bytes(node, 0xA5));
  EXPECT_EQ(0, rng.close_calls);
}

TEST(UuidNode, persistent_open_eintr_is_bounded)
{
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  rng.always_open_eintr = true;
  g_rng = &rng;
  const UuidCsprngOps ops = rng_ops();
  unsigned char node[UUID_NODE_LEN];
  fill_pattern(node, 0xA5);
  EXPECT_EQ(EINTR, fill_random_uuid_node(node, &ops));
  EXPECT_EQ(UUID_NODE_IO_RETRY_LIMIT + 1, rng.open_calls);
  EXPECT_EQ(0, rng.close_calls);
  EXPECT_TRUE(same_bytes(node, 0xA5));
}

TEST(UuidNode, partial_read_then_eof_is_entropy_failure)
{
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  rng.chunk = 3;
  rng.eof_after = 3;
  g_rng = &rng;
  const UuidCsprngOps ops = rng_ops();
  unsigned char node[UUID_NODE_LEN];
  fill_pattern(node, 0xA5);
  EXPECT_EQ(EIO, fill_random_uuid_node(node, &ops));
  EXPECT_TRUE(same_bytes(node, 0xA5));
  EXPECT_EQ(1, rng.close_calls);
}

TEST(UuidNode, read_error_and_persistent_read_eintr_do_not_publish)
{
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  rng.read_errno = EIO;
  rng.chunk = UUID_NODE_LEN;
  g_rng = &rng;
  const UuidCsprngOps ops = rng_ops();
  unsigned char node[UUID_NODE_LEN];
  fill_pattern(node, 0xA5);
  EXPECT_EQ(EIO, fill_random_uuid_node(node, &ops));
  EXPECT_TRUE(same_bytes(node, 0xA5));
  EXPECT_EQ(1, rng.close_calls);

  memset(&rng, 0, sizeof(rng));
  rng.always_read_eintr = true;
  g_rng = &rng;
  fill_pattern(node, 0xA5);
  EXPECT_EQ(EINTR, fill_random_uuid_node(node, &ops));
  EXPECT_EQ(UUID_NODE_IO_RETRY_LIMIT + 1, rng.read_calls);
  EXPECT_EQ(1, rng.close_calls);
  EXPECT_TRUE(same_bytes(node, 0xA5));
}

TEST(UuidNode, distinct_rng_streams_are_not_a_constant)
{
  unsigned char first_raw[UUID_NODE_LEN] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
  unsigned char second_raw[UUID_NODE_LEN] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25};
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  memcpy(rng.data, first_raw, UUID_NODE_LEN);
  rng.chunk = UUID_NODE_LEN;
  g_rng = &rng;
  const UuidCsprngOps ops = rng_ops();
  unsigned char first[UUID_NODE_LEN];
  unsigned char second[UUID_NODE_LEN];
  ASSERT_EQ(0, fill_random_uuid_node(first, &ops));
  rng.offset = 0;
  memcpy(rng.data, second_raw, UUID_NODE_LEN);
  ASSERT_EQ(0, fill_random_uuid_node(second, &ops));
  EXPECT_NE(0, memcmp(first, second, UUID_NODE_LEN));
  EXPECT_EQ(0x11, first[0]);
  EXPECT_EQ(0x21, second[0]);
}

TEST(UuidNode, mac_discovery_keeps_first_usable_address)
{
  const unsigned char lo_mac[UUID_NODE_LEN] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
  const unsigned char zero_mac[UUID_NODE_LEN] = {};
  const unsigned char eth_mac[UUID_NODE_LEN] = {0x02, 0x00, 0x00, 0x11, 0x22, 0x33};
  FakeNet net;
  memset(&net, 0, sizeof(net));
  set_nic(&net.nics[0], "lo", IFF_LOOPBACK | IFF_UP, lo_mac);
  set_nic(&net.nics[1], "tun0", IFF_UP, zero_mac);
  set_nic(&net.nics[2], "eth0", IFF_UP | IFF_RUNNING, eth_mac);
  net.nic_count = 3;
  g_net = &net;
  const UuidMacOps ops = net_ops();
  UuidMacDiscovery discovery;
  ASSERT_EQ(0, discover_linux_mac(&discovery, &ops));
  EXPECT_TRUE(discovery.found);
  EXPECT_EQ(0, memcmp(discovery.mac, eth_mac, UUID_NODE_LEN));
  EXPECT_EQ(0, discovery.discovery_errno);
  EXPECT_EQ(1, net.socket_calls);
  EXPECT_EQ(1, net.close_calls);
  EXPECT_EQ(0x02, discovery.mac[0]);
}

TEST(UuidNode, loopback_only_and_empty_list_have_no_usable_mac)
{
  const unsigned char lo_mac[UUID_NODE_LEN] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  FakeNet net;
  memset(&net, 0, sizeof(net));
  set_nic(&net.nics[0], "lo", IFF_LOOPBACK, lo_mac);
  net.nic_count = 1;
  g_net = &net;
  const UuidMacOps ops = net_ops();
  UuidMacDiscovery discovery;
  ASSERT_EQ(0, discover_linux_mac(&discovery, &ops));
  EXPECT_FALSE(discovery.found);
  EXPECT_EQ(0, discovery.discovery_errno);
  EXPECT_EQ(0, net.hw_calls);

  net.nic_count = 0;
  net.hw_calls = 0;
  ASSERT_EQ(0, discover_linux_mac(&discovery, &ops));
  EXPECT_FALSE(discovery.found);
  EXPECT_EQ(0, discovery.discovery_errno);
}

TEST(UuidNode, all_zero_hardware_address_is_skipped)
{
  const unsigned char zero_mac[UUID_NODE_LEN] = {};
  FakeNet net;
  memset(&net, 0, sizeof(net));
  set_nic(&net.nics[0], "tun0", IFF_UP, zero_mac);
  net.nic_count = 1;
  g_net = &net;
  const UuidMacOps ops = net_ops();
  UuidMacDiscovery discovery;
  ASSERT_EQ(0, discover_linux_mac(&discovery, &ops));
  EXPECT_FALSE(discovery.found);
  EXPECT_EQ(0, discovery.discovery_errno);
  EXPECT_EQ(1, net.close_calls);
}

TEST(UuidNode, socket_and_ifconf_eperm_are_captured_at_the_call)
{
  FakeNet net;
  memset(&net, 0, sizeof(net));
  net.socket_errno = EPERM;
  g_net = &net;
  const UuidMacOps ops = net_ops();
  UuidMacDiscovery discovery;
  ASSERT_EQ(0, discover_linux_mac(&discovery, &ops));
  EXPECT_FALSE(discovery.found);
  EXPECT_EQ(EPERM, discovery.discovery_errno);
  EXPECT_EQ(0, net.ioctl_calls);
  EXPECT_EQ(0, net.close_calls);

  memset(&net, 0, sizeof(net));
  net.conf_errno = EPERM;
  net.nic_count = 0;
  ASSERT_EQ(0, discover_linux_mac(&discovery, &ops));
  EXPECT_FALSE(discovery.found);
  EXPECT_EQ(EPERM, discovery.discovery_errno);
  EXPECT_EQ(1, net.close_calls);
}

TEST(UuidNode, per_interface_eperm_records_the_failing_name)
{
  const unsigned char eth_mac[UUID_NODE_LEN] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
  FakeNet net;
  memset(&net, 0, sizeof(net));
  set_nic(&net.nics[0], "eth0", IFF_UP, eth_mac);
  net.nics[0].hw_errno = EPERM;
  net.nic_count = 1;
  g_net = &net;
  const UuidMacOps ops = net_ops();
  UuidMacDiscovery discovery;
  ASSERT_EQ(0, discover_linux_mac(&discovery, &ops));
  EXPECT_FALSE(discovery.found);
  EXPECT_EQ(0, discovery.discovery_errno);
  EXPECT_EQ(EPERM, discovery.hw_errno);
  EXPECT_STREQ("eth0", discovery.failed_ifname);
  EXPECT_EQ(1, net.close_calls);

  memset(&net, 0, sizeof(net));
  set_nic(&net.nics[0], "eth1", IFF_UP, eth_mac);
  net.nics[0].flags_errno = EPERM;
  net.nic_count = 1;
  ASSERT_EQ(0, discover_linux_mac(&discovery, &ops));
  EXPECT_FALSE(discovery.found);
  EXPECT_EQ(EPERM, discovery.hw_errno);
  EXPECT_STREQ("eth1", discovery.failed_ifname);
  EXPECT_EQ(0, net.hw_calls);
}

TEST(UuidNode, later_interface_is_used_after_an_earlier_ioctl_error)
{
  const unsigned char bad_mac[UUID_NODE_LEN] = {0x02, 0x01, 0x01, 0x01, 0x01, 0x01};
  const unsigned char good_mac[UUID_NODE_LEN] = {0x02, 0x09, 0x08, 0x07, 0x06, 0x05};
  FakeNet net;
  memset(&net, 0, sizeof(net));
  set_nic(&net.nics[0], "eth0", IFF_UP, bad_mac);
  net.nics[0].flags_errno = EPERM;
  set_nic(&net.nics[1], "eth1", IFF_UP, good_mac);
  net.nic_count = 2;
  g_net = &net;
  const UuidMacOps ops = net_ops();
  UuidMacDiscovery discovery;
  ASSERT_EQ(0, discover_linux_mac(&discovery, &ops));
  EXPECT_TRUE(discovery.found);
  EXPECT_EQ(0, memcmp(discovery.mac, good_mac, UUID_NODE_LEN));
  EXPECT_EQ(0, discovery.hw_errno);
  EXPECT_STREQ("", discovery.failed_ifname);
}

TEST(UuidNode, usable_mac_wins_over_rng_failure)
{
  const unsigned char eth_mac[UUID_NODE_LEN] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  FakeNet net;
  memset(&net, 0, sizeof(net));
  set_nic(&net.nics[0], "eth0", IFF_UP, eth_mac);
  net.nic_count = 1;
  g_net = &net;
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  rng.fail_if_called = true;
  g_rng = &rng;
  const UuidMacOps mac = net_ops();
  const UuidCsprngOps rngops = rng_ops();
  unsigned char node[UUID_NODE_LEN];
  fill_pattern(node, 0xA5);
  bool used_random = true;
  UuidMacDiscovery discovery;
  ASSERT_EQ(0, acquire_linux_uuid_node(node, &used_random, &discovery, &mac, &rngops));
  EXPECT_FALSE(used_random);
  EXPECT_EQ(0, memcmp(node, eth_mac, UUID_NODE_LEN));
  EXPECT_EQ(0, rng.open_calls);
}

TEST(UuidNode, discovery_errors_fall_back_without_publishing_entropy_failure)
{
  FakeNet net;
  memset(&net, 0, sizeof(net));
  net.socket_errno = EPERM;
  g_net = &net;
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  unsigned char raw[UUID_NODE_LEN] = {0x40, 0x01, 0x02, 0x03, 0x04, 0x05};
  memcpy(rng.data, raw, UUID_NODE_LEN);
  rng.chunk = UUID_NODE_LEN;
  g_rng = &rng;
  const UuidMacOps mac = net_ops();
  const UuidCsprngOps rngops = rng_ops();
  unsigned char node[UUID_NODE_LEN];
  fill_pattern(node, 0xA5);
  bool used_random = false;
  UuidMacDiscovery discovery;
  ASSERT_EQ(0, acquire_linux_uuid_node(node, &used_random, &discovery, &mac, &rngops));
  EXPECT_TRUE(used_random);
  EXPECT_EQ(EPERM, discovery.discovery_errno);
  EXPECT_EQ(0x41, node[0]);
  EXPECT_EQ(0, memcmp(node + 1, raw + 1, UUID_NODE_LEN - 1));

  rng.open_errno = EIO;
  rng.offset = 0;
  fill_pattern(node, 0xA5);
  used_random = false;
  EXPECT_EQ(EIO, acquire_linux_uuid_node(node, &used_random, &discovery, &mac, &rngops));
  EXPECT_FALSE(used_random);
  EXPECT_TRUE(same_bytes(node, 0xA5));
}

TEST(UuidNode, no_usable_mac_and_injected_zero_mac_use_random_node)
{
  const unsigned char zero_mac[UUID_NODE_LEN] = {};
  FakeRng rng;
  memset(&rng, 0, sizeof(rng));
  unsigned char raw[UUID_NODE_LEN] = {0x7e, 0x10, 0x20, 0x30, 0x40, 0x50};
  memcpy(rng.data, raw, UUID_NODE_LEN);
  rng.chunk = UUID_NODE_LEN;
  g_rng = &rng;
  const UuidCsprngOps rngops = rng_ops();
  unsigned char node[UUID_NODE_LEN];
  bool used_random = false;
  // Android getifaddrs failure and a Linux loopback-only result share this path.
  ASSERT_EQ(0, finish_uuid_node(false, nullptr, node, &used_random, &rngops));
  EXPECT_TRUE(used_random);
  EXPECT_EQ(0x7f, node[0]);

  rng.offset = 0;
  used_random = false;
  ASSERT_EQ(0, finish_uuid_node(true, zero_mac, node, &used_random, &rngops));
  EXPECT_TRUE(used_random);
  expect_multicast(node);
}

TEST(UuidNode, production_node_is_stable_and_multicast_when_uncached)
{
  uuid_node_test_reset_cache();
  unsigned char first[UUID_NODE_LEN];
  unsigned char second[UUID_NODE_LEN];
  ASSERT_EQ(0, fill_random_uuid_node(first, nullptr));
  ASSERT_EQ(0, fill_random_uuid_node(second, nullptr));
  EXPECT_EQ(0, memcmp(first, second, UUID_NODE_LEN));
  expect_multicast(first);
  EXPECT_FALSE(same_bytes(first, 0));
}

TEST(UuidNode, concurrent_first_fill_publishes_one_node)
{
  uuid_node_test_reset_cache();
  constexpr int kThreads = 8;
  unsigned char nodes[kThreads][UUID_NODE_LEN];
  std::atomic<int> started{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      started.fetch_add(1, std::memory_order_acq_rel);
      while (started.load(std::memory_order_acquire) < kThreads) {
      }
      if (fill_random_uuid_node(nodes[i], nullptr) != 0) {
        errors.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(0, errors.load());
  expect_multicast(nodes[0]);
  for (int i = 1; i < kThreads; ++i) {
    EXPECT_EQ(0, memcmp(nodes[0], nodes[i], UUID_NODE_LEN));
  }
}

TEST(UuidNode, fork_inherits_the_published_node)
{
  unsigned char parent[UUID_NODE_LEN];
  ASSERT_EQ(0, fill_random_uuid_node(parent, nullptr));
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));
  const pid_t pid = fork();
  ASSERT_NE(-1, pid);
  if (pid == 0) {
    unsigned char child[UUID_NODE_LEN];
    const int rc = fill_random_uuid_node(child, nullptr);
    if (rc == 0) {
      (void)write(fds[1], child, UUID_NODE_LEN);
    }
    _exit(rc == 0 ? 0 : 1);
  }
  close(fds[1]);
  unsigned char child[UUID_NODE_LEN];
  size_t got = 0;
  while (got < static_cast<size_t>(UUID_NODE_LEN)) {
    const ssize_t n = read(fds[0], child + got, static_cast<size_t>(UUID_NODE_LEN) - got);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    got += static_cast<size_t>(n);
  }
  close(fds[0]);
  int status = 0;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
  ASSERT_EQ(static_cast<size_t>(UUID_NODE_LEN), got);
  EXPECT_EQ(0, memcmp(parent, child, UUID_NODE_LEN));
}

int read_exec_node(unsigned char *node)
{
  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) {
    return errno;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    const int err = errno;
    close(fds[0]);
    close(fds[1]);
    return err;
  }
  if (pid == 0) {
    if (dup2(fds[1], STDOUT_FILENO) < 0) {
      _exit(127);
    }
    close(fds[0]);
    close(fds[1]);
    char arg0[] = "test_uuid_node_fallback";
    char arg1[] = "--dump-uuid-node";
    char *argv[] = {arg0, arg1, nullptr};
    execve("/proc/self/exe", argv, environ);
    _exit(127);
  }
  close(fds[1]);
  size_t got = 0;
  while (got < static_cast<size_t>(UUID_NODE_LEN)) {
    const ssize_t n = read(fds[0], node + got, static_cast<size_t>(UUID_NODE_LEN) - got);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    got += static_cast<size_t>(n);
  }
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0 || got != static_cast<size_t>(UUID_NODE_LEN)) {
    return EIO;
  }
  return 0;
}

TEST(UuidNode, independent_processes_do_not_share_a_constant_node)
{
  unsigned char first[UUID_NODE_LEN];
  unsigned char second[UUID_NODE_LEN];
  ASSERT_EQ(0, read_exec_node(first));
  ASSERT_EQ(0, read_exec_node(second));
  expect_multicast(first);
  expect_multicast(second);
  EXPECT_NE(0, memcmp(first, second, UUID_NODE_LEN));
}

struct NetnsResult {
  int discover_rc;
  int found;
  int discovery_errno;
  int hw_errno;
  int acquire_rc;
  int used_random;
  unsigned char node[UUID_NODE_LEN];
};

TEST(UuidNode, fresh_netns_without_a_mac_still_initializes)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));
  const pid_t pid = fork();
  ASSERT_NE(-1, pid);
  if (pid == 0) {
    if (unshare(CLONE_NEWNET) != 0) {
      const int err = errno;
      (void)write(fds[1], &err, sizeof(err));
      _exit(2);
    }
    NetnsResult result;
    memset(&result, 0, sizeof(result));
    UuidMacDiscovery discovery;
    result.discover_rc = discover_linux_mac(&discovery, nullptr);
    result.found = discovery.found ? 1 : 0;
    result.discovery_errno = discovery.discovery_errno;
    result.hw_errno = discovery.hw_errno;
    bool used_random = false;
    result.acquire_rc = acquire_linux_uuid_node(
        result.node, &used_random, &discovery, nullptr, nullptr);
    result.used_random = used_random ? 1 : 0;
    (void)write(fds[1], &result, sizeof(result));
    _exit(0);
  }
  close(fds[1]);
  unsigned char buf[sizeof(NetnsResult)];
  size_t got = 0;
  while (got < sizeof(buf)) {
    const ssize_t n = read(fds[0], buf + got, sizeof(buf) - got);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    got += static_cast<size_t>(n);
  }
  close(fds[0]);
  int status = 0;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_TRUE(WIFEXITED(status));
  if (WEXITSTATUS(status) == 2) {
    int err = 0;
    ASSERT_GE(got, sizeof(err));
    memcpy(&err, buf, sizeof(err));
    GTEST_SKIP() << "CLONE_NEWNET is unavailable, errno=" << err;
  }
  ASSERT_EQ(0, WEXITSTATUS(status));
  ASSERT_EQ(sizeof(NetnsResult), got);
  NetnsResult result;
  memcpy(&result, buf, sizeof(result));
  EXPECT_EQ(0, result.discover_rc);
  EXPECT_EQ(0, result.found);
  EXPECT_TRUE(result.discovery_errno == 0 || result.discovery_errno == EPERM ||
              result.discovery_errno == EACCES);
  EXPECT_EQ(0, result.acquire_rc);
  EXPECT_EQ(1, result.used_random);
  expect_multicast(result.node);
}

TEST(UuidNode, live_host_node_is_stable)
{
  unsigned char first[UUID_NODE_LEN];
  unsigned char second[UUID_NODE_LEN];
  bool random_first = false;
  bool random_second = false;
  UuidMacDiscovery first_discovery;
  UuidMacDiscovery second_discovery;
  ASSERT_EQ(0, acquire_linux_uuid_node(first, &random_first, &first_discovery, nullptr, nullptr));
  ASSERT_EQ(0, acquire_linux_uuid_node(second, &random_second, &second_discovery, nullptr, nullptr));
  EXPECT_EQ(random_first, random_second);
  EXPECT_EQ(0, memcmp(first, second, UUID_NODE_LEN));
  if (random_first) {
    EXPECT_FALSE(first_discovery.found);
    expect_multicast(first);
  } else {
    EXPECT_TRUE(first_discovery.found);
    EXPECT_TRUE(uuid_node_mac_usable(first));
    EXPECT_EQ(0, memcmp(first, first_discovery.mac, UUID_NODE_LEN));
  }
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  if (argc > 1 && std::strcmp(argv[1], "--dump-uuid-node") == 0) {
    unsigned char node[oceanbase::sql::UUID_NODE_LEN];
    if (oceanbase::sql::fill_random_uuid_node(node, nullptr) != 0) {
      return 1;
    }
    return std::fwrite(node, 1, sizeof(node), stdout) == sizeof(node) ? 0 : 1;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
