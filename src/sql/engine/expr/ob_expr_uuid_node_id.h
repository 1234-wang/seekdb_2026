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

#ifndef SRC_SQL_ENGINE_EXPR_OB_EXPR_UUID_NODE_ID_H_
#define SRC_SQL_ENGINE_EXPR_OB_EXPR_UUID_NODE_ID_H_

#include <stdint.h>

namespace oceanbase
{
namespace sql
{
namespace uuid_node_id
{

/*
 * Resolution of the 48-bit node identifier embedded in version-1 UUIDs.
 *
 * RFC 4122 section 4.1.6 defines the node identifier as the IEEE 802 address of
 * the host, section 4.5 allows a random value with the multicast bit set when
 * such an address cannot be observed.  The functions below implement both, in
 * that order of preference.
 *
 * This translation unit deliberately has no OceanBase dependency: it is plain
 * POSIX so that the policy can be linked and driven directly by unit tests.
 * The caller maps the Result values onto OceanBase error codes.
 */

const int64_t NODE_ID_LEN = 6;
const int64_t IFNAME_MAX_LEN = 32;

enum Result
{
  RESULT_OK = 0,
  RESULT_INVALID_ARGUMENT = 1,
  RESULT_ENTROPY_UNAVAILABLE = 2,  // random node id needed, entropy source unusable
};

enum Source
{
  SOURCE_NONE = 0,
  SOURCE_IOCTL = 1,       // SIOCGIFCONF + SIOCGIFFLAGS + SIOCGIFHWADDR
  SOURCE_GETIFADDRS = 2,  // getifaddrs() with AF_PACKET link-layer addresses
  SOURCE_RANDOM = 3,      // RFC 4122 section 4.5 random node id
};

enum Step
{
  STEP_NONE = 0,
  STEP_SOCKET = 1,
  STEP_SIOCGIFCONF = 2,
  STEP_SIOCGIFFLAGS = 3,
  STEP_SIOCGIFHWADDR = 4,
  STEP_GETIFADDRS = 5,
  STEP_ENTROPY_OPEN = 6,
  STEP_ENTROPY_READ = 7,
};

const char *source_name(Source source);
const char *step_name(Step step);

// Printed through the K() macro, so that one log line carries both the outcome
// and the reason the losing strategies gave up.
struct Diagnostics
{
public:
  Diagnostics();
  void reset();
  int64_t to_string(char *buf, const int64_t buf_len) const;

  Source source_;
  Step failed_step_;          // first failing enumeration step, STEP_NONE when nothing failed
  int32_t failed_errno_;      // errno of failed_step_, 0 when nothing failed
  int32_t iface_count_;       // interfaces reported by the enumerations
  int32_t rejected_iface_count_;  // interfaces skipped: loopback, or no usable address
  Step entropy_step_;         // failing step of the random fallback, STEP_NONE when unused
  int32_t entropy_errno_;     // errno of entropy_step_
  char ifname_[IFNAME_MAX_LEN];   // interface that contributed the node id
};

// Knobs that exist so tests can pin a specific strategy without breaking the
// host's networking.  Production always uses the defaults.
struct Policy
{
public:
  Policy();
  Policy(const bool allow_ioctl, const bool allow_getifaddrs, const char *entropy_path);

  bool allow_ioctl_;
  bool allow_getifaddrs_;
  const char *entropy_path_;
};

// Resolve with the production policy: ioctl, then getifaddrs, then the random
// node identifier.
Result resolve(unsigned char *node_id, const int64_t node_id_len, Diagnostics &diag);

Result resolve_with_policy(unsigned char *node_id,
                           const int64_t node_id_len,
                           Diagnostics &diag,
                           const Policy &policy);

// An all-zero link-layer address is what the kernel reports for interfaces
// without hardware addresses (tun devices, unconfigured veth pairs, ...), so it
// must never be adopted as the node identifier.
bool is_usable_link_layer_addr(const unsigned char *addr);

}  // namespace uuid_node_id
}  // namespace sql
}  // namespace oceanbase

#endif  // SRC_SQL_ENGINE_EXPR_OB_EXPR_UUID_NODE_ID_H_
