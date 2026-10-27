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

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "sql/engine/expr/ob_expr_uuid_node_id.h"

namespace oceanbase
{
namespace sql
{
namespace uuid_node_id
{

namespace
{

// A file that exists but holds fewer than six bytes: read() returns a short
// count and hits EOF, which must not be mistaken for entropy.
class ShortEntropyFile
{
public:
  ShortEntropyFile() : fd_(-1)
  {
    char pattern[] = "/tmp/uuid_node_id_short_XXXXXX";
    fd_ = ::mkstemp(pattern);
    if (fd_ >= 0) {
      path_ = pattern;
      const ssize_t n = ::write(fd_, "abc", 3);
      (void)n;
      ::close(fd_);
      fd_ = -1;
    }
  }
  ~ShortEntropyFile()
  {
    if (!path_.empty()) {
      ::unlink(path_.c_str());
    }
  }
  const char *path() const { return path_.c_str(); }
  bool valid() const { return !path_.empty(); }

private:
  int fd_;
  std::string path_;
};

class NamedPipe
{
public:
  NamedPipe() : created_(false)
  {
    char pattern[] = "/tmp/uuid_node_id_fifo_XXXXXX";
    const int fd = ::mkstemp(pattern);
    if (fd >= 0) {
      ::close(fd);
      ::unlink(pattern);
      if (0 == ::mkfifo(pattern, 0600)) {
        path_ = pattern;
        created_ = true;
      }
    }
  }
  ~NamedPipe()
  {
    if (created_) {
      ::unlink(path_.c_str());
    }
  }
  const char *path() const { return path_.c_str(); }
  bool valid() const { return created_; }

private:
  std::string path_;
  bool created_;
};

// The writer stays open until every byte is delivered, so the reader observes
// one byte per read() and has to accumulate them.
void dribble_bytes(const char *path, const char *bytes, const int len)
{
  const int fd = ::open(path, O_WRONLY);
  if (fd >= 0) {
    for (int i = 0; i < len; ++i) {
      const ssize_t n = ::write(fd, bytes + i, 1);
      (void)n;
      ::usleep(1000);
    }
    ::close(fd);
  }
}

// Tests that need a node id to exist can only run where the environment offers
// one: a host with no visible interface *and* no entropy has nothing to resolve,
// which the dedicated entropy tests cover instead.
bool entropy_is_available()
{
  unsigned char node_id[NODE_ID_LEN] = {0};
  Diagnostics diag;
  return RESULT_OK == resolve_with_policy(node_id, NODE_ID_LEN, diag,
                                          Policy(false, false, "/dev/urandom"));
}

std::string hex(const unsigned char *node_id)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
           node_id[0], node_id[1], node_id[2], node_id[3], node_id[4], node_id[5]);
  return std::string(buf);
}

}  // namespace

// The historical ioctl path stays authoritative.  Whatever enumeration this
// host permits, the two strategies must agree on the bytes: the node id may not
// depend on which of them produced it, otherwise a sandbox that filters
// SIOCGIFCONF would silently change every uuid() emitted by the server.
TEST(UuidNodeId, hardware_address_is_consistent_across_strategies)
{
  if (!entropy_is_available()) {
    std::cerr << "skip: this environment offers neither an interface nor entropy" << std::endl;
    return;
  }
  unsigned char via_default[NODE_ID_LEN] = {0};
  unsigned char via_ioctl[NODE_ID_LEN] = {0};
  unsigned char via_getifaddrs[NODE_ID_LEN] = {0};
  Diagnostics by_default;
  Diagnostics by_ioctl;
  Diagnostics by_getifaddrs;

  ASSERT_EQ(RESULT_OK, resolve(via_default, NODE_ID_LEN, by_default));
  EXPECT_TRUE(is_usable_link_layer_addr(via_default));

  const Result ioctl_result =
      resolve_with_policy(via_ioctl, NODE_ID_LEN, by_ioctl, Policy(true, false, "/dev/urandom"));
  const Result getifaddrs_result = resolve_with_policy(via_getifaddrs, NODE_ID_LEN, by_getifaddrs,
                                                       Policy(false, true, "/dev/urandom"));
  ASSERT_EQ(RESULT_OK, ioctl_result);
  ASSERT_EQ(RESULT_OK, getifaddrs_result);
  EXPECT_TRUE(is_usable_link_layer_addr(via_ioctl));
  EXPECT_TRUE(is_usable_link_layer_addr(via_getifaddrs));

  if (SOURCE_IOCTL == by_default.source_) {
    EXPECT_EQ(0, memcmp(via_default, via_ioctl, NODE_ID_LEN))
        << "ioctl path is authoritative, default resolution must return the same bytes";
  }
  if (SOURCE_IOCTL == by_ioctl.source_ && SOURCE_GETIFADDRS == by_getifaddrs.source_) {
    EXPECT_EQ(0, memcmp(via_ioctl, via_getifaddrs, NODE_ID_LEN))
        << "ioctl=" << hex(via_ioctl) << " getifaddrs=" << hex(via_getifaddrs)
        << " (ifname " << by_ioctl.ifname_ << " vs " << by_getifaddrs.ifname_ << ")";
  }
}

// Resolving from a hardware address must be a pure function of the interface
// table, otherwise a restart would silently change every uuid() the server
// emits.  The random variant is deliberately *not* stable here: it is redrawn
// per call, and the product resolves it exactly once (init_sql_expr_static_var)
// and keeps it in ObUUIDNode::mac_addr_ for the lifetime of the process.
TEST(UuidNodeId, hardware_address_resolution_is_stable)
{
  if (!entropy_is_available()) {
    std::cerr << "skip: this environment offers neither an interface nor entropy" << std::endl;
    return;
  }
  unsigned char first[NODE_ID_LEN] = {0};
  unsigned char next[NODE_ID_LEN] = {0};
  Diagnostics diag;

  ASSERT_EQ(RESULT_OK, resolve(first, NODE_ID_LEN, diag));
  if (SOURCE_RANDOM == diag.source_) {
    std::cerr << "no hardware address in this environment, stability is not defined" << std::endl;
    return;
  }
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(RESULT_OK, resolve(next, NODE_ID_LEN, diag));
    EXPECT_EQ(0, memcmp(first, next, NODE_ID_LEN));
  }
}

TEST(UuidNodeId, random_node_id_when_mac_is_unavailable)
{
  if (!entropy_is_available()) {
    std::cerr << "skip: no entropy source in this environment" << std::endl;
    return;
  }
  unsigned char node_id[NODE_ID_LEN] = {0};
  Diagnostics diag;

  ASSERT_EQ(RESULT_OK,
            resolve_with_policy(node_id, NODE_ID_LEN, diag, Policy(false, false, "/dev/urandom")));
  EXPECT_EQ(SOURCE_RANDOM, diag.source_);
  EXPECT_TRUE(is_usable_link_layer_addr(node_id));
  EXPECT_EQ(0x01, node_id[0] & 0x01) << "RFC 4122 section 4.5 requires the multicast bit";
}

// The maintainer's requirement: a genuine entropy failure stays an
// initialisation error.  No clock/pid derived substitute may be produced.
TEST(UuidNodeId, unavailable_entropy_source_is_an_error)
{
  unsigned char node_id[NODE_ID_LEN] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  Diagnostics diag;

  EXPECT_EQ(RESULT_ENTROPY_UNAVAILABLE,
            resolve_with_policy(node_id, NODE_ID_LEN, diag,
                                Policy(false, false, "/nonexistent/uuid-node-id-entropy")));
  EXPECT_EQ(STEP_ENTROPY_OPEN, diag.entropy_step_);
  EXPECT_EQ(ENOENT, diag.entropy_errno_);
  EXPECT_EQ(STEP_NONE, diag.failed_step_) << "no enumeration was attempted";
  EXPECT_EQ(SOURCE_NONE, diag.source_);
}

TEST(UuidNodeId, short_entropy_read_is_an_error)
{
  ShortEntropyFile file;
  ASSERT_TRUE(file.valid()) << "cannot create the temporary file";
  unsigned char node_id[NODE_ID_LEN] = {0};
  Diagnostics diag;

  EXPECT_EQ(RESULT_ENTROPY_UNAVAILABLE,
            resolve_with_policy(node_id, NODE_ID_LEN, diag,
                                Policy(false, false, file.path())));
  EXPECT_EQ(STEP_ENTROPY_READ, diag.entropy_step_);
}

TEST(UuidNodeId, partial_entropy_reads_are_accumulated)
{
  NamedPipe pipe;
  if (!pipe.valid()) {
    // Kept out of the failure set: some container images mount /tmp without
    // support for FIFOs, which says nothing about the resolver.
    std::cerr << "mkfifo unavailable, skipping partial read test" << std::endl;
    return;
  }
  const char bytes[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  std::thread writer(dribble_bytes, pipe.path(), bytes, 6);

  unsigned char node_id[NODE_ID_LEN] = {0};
  Diagnostics diag;
  const Result result =
      resolve_with_policy(node_id, NODE_ID_LEN, diag, Policy(false, false, pipe.path()));
  writer.join();

  ASSERT_EQ(RESULT_OK, result) << "diagnostics: " << step_name(diag.entropy_step_);
  EXPECT_EQ(SOURCE_RANDOM, diag.source_);
  // The multicast bit is set by the resolver, the remaining five bytes are the
  // entropy source's.
  EXPECT_EQ(0x11 | 0x01, node_id[0]);
  EXPECT_EQ(0, memcmp(node_id + 1, bytes + 1, NODE_ID_LEN - 1));
}

TEST(UuidNodeId, zero_link_layer_address_is_rejected)
{
  const unsigned char zero[NODE_ID_LEN] = {0, 0, 0, 0, 0, 0};
  const unsigned char one[NODE_ID_LEN] = {0, 0, 0, 0, 0, 1};
  const unsigned char real[NODE_ID_LEN] = {0x02, 0x42, 0xac, 0x11, 0x00, 0x02};

  EXPECT_FALSE(is_usable_link_layer_addr(zero));
  EXPECT_FALSE(is_usable_link_layer_addr(NULL));
  EXPECT_TRUE(is_usable_link_layer_addr(one));
  EXPECT_TRUE(is_usable_link_layer_addr(real));
}

TEST(UuidNodeId, invalid_arguments_are_reported)
{
  unsigned char node_id[NODE_ID_LEN] = {0};
  Diagnostics diag;

  EXPECT_EQ(RESULT_INVALID_ARGUMENT, resolve(NULL, NODE_ID_LEN, diag));
  EXPECT_EQ(RESULT_INVALID_ARGUMENT, resolve(node_id, NODE_ID_LEN - 1, diag));
  EXPECT_EQ(SOURCE_NONE, diag.source_);
}

TEST(UuidNodeId, diagnostics_are_printable_and_bounded)
{
  if (!entropy_is_available()) {
    std::cerr << "skip: this environment offers neither an interface nor entropy" << std::endl;
    return;
  }
  unsigned char node_id[NODE_ID_LEN] = {0};
  Diagnostics diag;
  ASSERT_EQ(RESULT_OK, resolve(node_id, NODE_ID_LEN, diag));

  char buf[256];
  const int64_t len = diag.to_string(buf, sizeof(buf));
  ASSERT_GT(len, 0);
  EXPECT_NE(nullptr, strstr(buf, "source:"));
  EXPECT_NE(nullptr, strstr(buf, "failed_step:"));

  char small[8];
  const int64_t small_len = diag.to_string(small, sizeof(small));
  EXPECT_GT(small_len, 0);
  EXPECT_LT(small_len, static_cast<int64_t>(sizeof(small)));
}

// uuid() is called concurrently by every session; the random path must not
// hand out duplicates.
TEST(UuidNodeId, random_node_ids_are_unique_across_threads)
{
  if (!entropy_is_available()) {
    std::cerr << "skip: no entropy source in this environment" << std::endl;
    return;
  }
  const int thread_count = 8;
  const int per_thread = 32;
  std::vector<std::vector<std::string>> produced(thread_count);
  std::atomic<int> failures(0);

  std::vector<std::thread> threads;
  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([t, &produced, &failures]() {
      for (int i = 0; i < per_thread; ++i) {
        unsigned char node_id[NODE_ID_LEN] = {0};
        Diagnostics diag;
        if (RESULT_OK != resolve_with_policy(node_id, NODE_ID_LEN, diag,
                                             Policy(false, false, "/dev/urandom"))) {
          ++failures;
          continue;
        }
        produced[t].push_back(hex(node_id));
      }
    });
  }
  for (int t = 0; t < thread_count; ++t) {
    threads[t].join();
  }

  EXPECT_EQ(0, failures.load());
  std::vector<std::string> all;
  for (int t = 0; t < thread_count; ++t) {
    all.insert(all.end(), produced[t].begin(), produced[t].end());
  }
  ASSERT_EQ(static_cast<size_t>(thread_count * per_thread), all.size());
  std::sort(all.begin(), all.end());
  EXPECT_EQ(all.end(), std::unique(all.begin(), all.end()))
      << "duplicate random node ids were produced";
}

}  // namespace uuid_node_id
}  // namespace sql
}  // namespace oceanbase

#endif  // defined(__linux__)
