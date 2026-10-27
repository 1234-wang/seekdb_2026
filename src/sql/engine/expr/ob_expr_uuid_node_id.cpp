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

#include "sql/engine/expr/ob_expr_uuid_node_id.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace oceanbase
{
namespace sql
{
namespace uuid_node_id
{

const char *source_name(const Source source)
{
  const char *name = "UNKNOWN";
  switch (source) {
    case SOURCE_NONE:
      name = "NONE";
      break;
    case SOURCE_IOCTL:
      name = "IOCTL";
      break;
    case SOURCE_GETIFADDRS:
      name = "GETIFADDRS";
      break;
    case SOURCE_RANDOM:
      name = "RANDOM";
      break;
    default:
      break;
  }
  return name;
}

const char *step_name(const Step step)
{
  const char *name = "UNKNOWN";
  switch (step) {
    case STEP_NONE:
      name = "NONE";
      break;
    case STEP_SOCKET:
      name = "socket(AF_INET, SOCK_DGRAM)";
      break;
    case STEP_SIOCGIFCONF:
      name = "ioctl(SIOCGIFCONF)";
      break;
    case STEP_SIOCGIFFLAGS:
      name = "ioctl(SIOCGIFFLAGS)";
      break;
    case STEP_SIOCGIFHWADDR:
      name = "ioctl(SIOCGIFHWADDR)";
      break;
    case STEP_GETIFADDRS:
      name = "getifaddrs";
      break;
    case STEP_ENTROPY_OPEN:
      name = "open(entropy)";
      break;
    case STEP_ENTROPY_READ:
      name = "read(entropy)";
      break;
    default:
      break;
  }
  return name;
}

Diagnostics::Diagnostics()
{
  reset();
}

void Diagnostics::reset()
{
  source_ = SOURCE_NONE;
  failed_step_ = STEP_NONE;
  failed_errno_ = 0;
  iface_count_ = 0;
  rejected_iface_count_ = 0;
  entropy_step_ = STEP_NONE;
  entropy_errno_ = 0;
  ifname_[0] = '\0';
}

int64_t Diagnostics::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  if (NULL != buf && buf_len > 0) {
    int64_t n = snprintf(buf, buf_len,
                         "{source:%s, ifname:\"%.31s\", iface_count:%d, rejected_iface:%d, "
                         "failed_step:\"%s\", failed_errno:%d",
                         source_name(source_), ifname_, iface_count_, rejected_iface_count_,
                         step_name(failed_step_), failed_errno_);
    pos = (n < 0) ? 0 : ((n >= buf_len) ? (buf_len - 1) : n);
    if (0 != failed_errno_ && pos < buf_len) {
      // strerror() keeps the errno actionable without the reader looking it up.
      n = snprintf(buf + pos, buf_len - pos, "(%s)", strerror(failed_errno_));
      pos += (n < 0) ? 0 : ((n >= buf_len - pos) ? (buf_len - pos - 1) : n);
    }
    if (STEP_NONE != entropy_step_ && pos < buf_len) {
      n = snprintf(buf + pos, buf_len - pos, ", entropy_step:\"%s\", entropy_errno:%d",
                   step_name(entropy_step_), entropy_errno_);
      pos += (n < 0) ? 0 : ((n >= buf_len - pos) ? (buf_len - pos - 1) : n);
      if (0 != entropy_errno_ && pos < buf_len) {
        n = snprintf(buf + pos, buf_len - pos, "(%s)", strerror(entropy_errno_));
        pos += (n < 0) ? 0 : ((n >= buf_len - pos) ? (buf_len - pos - 1) : n);
      }
    }
    if (pos < buf_len) {
      n = snprintf(buf + pos, buf_len - pos, "}");
      pos += (n < 0) ? 0 : ((n >= buf_len - pos) ? (buf_len - pos - 1) : n);
    }
  }
  return pos;
}

Policy::Policy()
    : allow_ioctl_(true), allow_getifaddrs_(true), entropy_path_("/dev/urandom")
{
}

Policy::Policy(const bool allow_ioctl, const bool allow_getifaddrs, const char *entropy_path)
    : allow_ioctl_(allow_ioctl),
      allow_getifaddrs_(allow_getifaddrs),
      entropy_path_(NULL == entropy_path ? "/dev/urandom" : entropy_path)
{
}

bool is_usable_link_layer_addr(const unsigned char *addr)
{
  bool usable = (NULL != addr);
  bool all_zero = true;
  for (int64_t i = 0; usable && i < NODE_ID_LEN; ++i) {
    if (0 != addr[i]) {
      all_zero = false;
    }
  }
  return usable && !all_zero;
}

namespace
{

// The first failure explains why a strategy gave up; later ones are follow-up
// noise and would overwrite the actionable information.
void set_failed_step(Diagnostics &diag, const Step step, const int err)
{
  if (STEP_NONE == diag.failed_step_) {
    diag.failed_step_ = step;
    diag.failed_errno_ = err;
  }
}

void set_entropy_failure(Diagnostics &diag, const Step step, const int err)
{
  if (STEP_NONE == diag.entropy_step_) {
    diag.entropy_step_ = step;
    diag.entropy_errno_ = err;
  }
}

void adopt_node_id(unsigned char *node_id,
                   const unsigned char *addr,
                   const char *ifname,
                   Diagnostics &diag)
{
  memcpy(node_id, addr, static_cast<size_t>(NODE_ID_LEN));
  if (NULL != ifname) {
    snprintf(diag.ifname_, sizeof(diag.ifname_), "%s", ifname);
  }
}

// Strategy 1: the historical implementation.  It only reports AF_INET
// configured interfaces and requires the SIOCGIF* ioctls to be permitted.
bool probe_node_id_via_ioctl(unsigned char *node_id, Diagnostics &diag)
{
  bool found = false;
  int sock = -1;
  char buf[4096];
  struct ifconf ifc;

  if ((sock = ::socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    set_failed_step(diag, STEP_SOCKET, errno);
  } else {
    memset(buf, 0, sizeof(buf));
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifc_len = static_cast<int>(sizeof(buf));
    ifc.ifc_buf = buf;
    if (0 != ::ioctl(sock, SIOCGIFCONF, &ifc)) {
      set_failed_step(diag, STEP_SIOCGIFCONF, errno);
    } else {
      const int32_t iface_count = ifc.ifc_len / static_cast<int32_t>(sizeof(struct ifreq));
      diag.iface_count_ += iface_count;
      for (int32_t i = 0; !found && i < iface_count; ++i) {
        // ifr is rebuilt for every interface: SIOCGIFFLAGS only writes ifr_flags,
        // so a name longer than IFNAMSIZ - 1 must not carry over from the
        // previous iteration.
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        if ('\0' == ifc.ifc_req[i].ifr_name[0]) {
          continue;
        }
        strncpy(ifr.ifr_name, ifc.ifc_req[i].ifr_name, IFNAMSIZ - 1);
        if (0 != ::ioctl(sock, SIOCGIFFLAGS, &ifr)) {
          set_failed_step(diag, STEP_SIOCGIFFLAGS, errno);
          ++diag.rejected_iface_count_;
        } else if (0 != (ifr.ifr_flags & IFF_LOOPBACK)) {
          ++diag.rejected_iface_count_;
        } else if (0 != ::ioctl(sock, SIOCGIFHWADDR, &ifr)) {
          set_failed_step(diag, STEP_SIOCGIFHWADDR, errno);
          ++diag.rejected_iface_count_;
        } else {
          const unsigned char *addr =
              reinterpret_cast<const unsigned char *>(ifr.ifr_hwaddr.sa_data);
          if (is_usable_link_layer_addr(addr)) {
            adopt_node_id(node_id, addr, ifr.ifr_name, diag);
            found = true;
          } else {
            ++diag.rejected_iface_count_;
          }
        }
      }
    }
    ::close(sock);
  }
  return found;
}

// Strategy 2: getifaddrs() obtains the same information through netlink, so it
// still works where the SIOCGIF* ioctls are filtered, and it is not bounded by a
// caller supplied buffer.  Some sandboxes filter netlink instead, which is why
// this is a fallback rather than a replacement.
bool probe_node_id_via_getifaddrs(unsigned char *node_id, Diagnostics &diag)
{
  bool found = false;
  struct ifaddrs *ifaddr = NULL;

  if (0 != ::getifaddrs(&ifaddr)) {
    set_failed_step(diag, STEP_GETIFADDRS, errno);
  } else {
    for (struct ifaddrs *ifa = ifaddr; NULL != ifa && !found; ifa = ifa->ifa_next) {
      const struct sockaddr *sa = ifa->ifa_addr;
      if (NULL == sa || AF_PACKET != sa->sa_family) {
        continue;
      }
      ++diag.iface_count_;
      const struct sockaddr_ll *sll = reinterpret_cast<const struct sockaddr_ll *>(sa);
      if (0 != (ifa->ifa_flags & IFF_LOOPBACK)) {
        ++diag.rejected_iface_count_;
      } else if (NODE_ID_LEN != sll->sll_halen) {
        ++diag.rejected_iface_count_;
      } else if (!is_usable_link_layer_addr(sll->sll_addr)) {
        ++diag.rejected_iface_count_;
      } else {
        adopt_node_id(node_id, sll->sll_addr, ifa->ifa_name, diag);
        found = true;
      }
    }
    ::freeifaddrs(ifaddr);
  }
  return found;
}

// Strategy 3: RFC 4122 section 4.5 random node identifier.  The multicast bit
// marks the value as "not an IEEE 802 address", so UUID consumers can tell the
// two variants apart.  A failure of the entropy source is a genuine
// initialisation failure: silently substituting a weaker value (the clock, the
// pid, ...) would hand out node identifiers that are predictable and that two
// hosts starting in the same tick could share.
bool fill_random_node_id(unsigned char *node_id, Diagnostics &diag, const char *entropy_path)
{
  bool ok = false;
  int64_t got = 0;
  int fd = -1;

  if (NULL == entropy_path || '\0' == entropy_path[0]) {
    set_entropy_failure(diag, STEP_ENTROPY_OPEN, EINVAL);
  } else if ((fd = ::open(entropy_path, O_RDONLY | O_CLOEXEC)) < 0) {
    set_entropy_failure(diag, STEP_ENTROPY_OPEN, errno);
  } else {
    // A short read is a failure, not a partial node id: only a fully random
    // 48-bit value satisfies RFC 4122 section 4.5.
    while (got < NODE_ID_LEN) {
      const ssize_t n = ::read(fd, node_id + got, static_cast<size_t>(NODE_ID_LEN - got));
      if (n > 0) {
        got += n;
      } else if (0 == n) {
        set_entropy_failure(diag, STEP_ENTROPY_READ, 0);
        break;
      } else if (EINTR != errno) {
        set_entropy_failure(diag, STEP_ENTROPY_READ, errno);
        break;
      }
    }
    ::close(fd);
    ok = (NODE_ID_LEN == got);
  }
  if (ok) {
    node_id[0] = static_cast<unsigned char>(node_id[0] | 0x01);
  }
  return ok;
}

}  // namespace

Result resolve_with_policy(unsigned char *node_id,
                           const int64_t node_id_len,
                           Diagnostics &diag,
                           const Policy &policy)
{
  Result result = RESULT_OK;
  diag.reset();
  if (NULL == node_id || NODE_ID_LEN != node_id_len) {
    result = RESULT_INVALID_ARGUMENT;
  } else if (policy.allow_ioctl_ && probe_node_id_via_ioctl(node_id, diag)) {
    diag.source_ = SOURCE_IOCTL;
  } else if (policy.allow_getifaddrs_ && probe_node_id_via_getifaddrs(node_id, diag)) {
    diag.source_ = SOURCE_GETIFADDRS;
  } else if (!fill_random_node_id(node_id, diag, policy.entropy_path_)) {
    result = RESULT_ENTROPY_UNAVAILABLE;
  } else {
    diag.source_ = SOURCE_RANDOM;
  }
  return result;
}

Result resolve(unsigned char *node_id, const int64_t node_id_len, Diagnostics &diag)
{
  return resolve_with_policy(node_id, node_id_len, diag, Policy());
}

}  // namespace uuid_node_id
}  // namespace sql
}  // namespace oceanbase

#else  // defined(__linux__)

namespace oceanbase
{
namespace sql
{
namespace uuid_node_id
{

const char *source_name(const Source source)
{
  (void)source;
  return "UNKNOWN";
}

const char *step_name(const Step step)
{
  (void)step;
  return "UNKNOWN";
}

Diagnostics::Diagnostics()
{
  reset();
}

void Diagnostics::reset()
{
  source_ = SOURCE_NONE;
  failed_step_ = STEP_NONE;
  failed_errno_ = 0;
  iface_count_ = 0;
  rejected_iface_count_ = 0;
  entropy_step_ = STEP_NONE;
  entropy_errno_ = 0;
  ifname_[0] = '\0';
}

int64_t Diagnostics::to_string(char *buf, const int64_t buf_len) const
{
  (void)buf;
  (void)buf_len;
  return 0;
}

Policy::Policy()
    : allow_ioctl_(true), allow_getifaddrs_(true), entropy_path_("/dev/urandom")
{
}

Policy::Policy(const bool allow_ioctl, const bool allow_getifaddrs, const char *entropy_path)
    : allow_ioctl_(allow_ioctl),
      allow_getifaddrs_(allow_getifaddrs),
      entropy_path_(entropy_path)
{
}

bool is_usable_link_layer_addr(const unsigned char *addr)
{
  (void)addr;
  return false;
}

Result resolve_with_policy(unsigned char *node_id,
                           const int64_t node_id_len,
                           Diagnostics &diag,
                           const Policy &policy)
{
  (void)node_id;
  (void)node_id_len;
  (void)policy;
  diag.reset();
  return RESULT_INVALID_ARGUMENT;
}

Result resolve(unsigned char *node_id, const int64_t node_id_len, Diagnostics &diag)
{
  (void)node_id;
  (void)node_id_len;
  diag.reset();
  return RESULT_INVALID_ARGUMENT;
}

}  // namespace uuid_node_id
}  // namespace sql
}  // namespace oceanbase

#endif  // defined(__linux__)
