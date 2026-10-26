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

#include "sql/engine/expr/ob_uuid_node.h"

#include <errno.h>
#include <string.h>

#include <atomic>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <pthread.h>
#endif

#if defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

namespace oceanbase
{
namespace sql
{

namespace
{

int uuid_node_unsupported()
{
#if defined(ENOTSUP)
  return ENOTSUP;
#elif defined(EOPNOTSUPP)
  return EOPNOTSUPP;
#else
  return EINVAL;
#endif
}

#if !defined(_WIN32)

int uuid_node_sys_open(const char *path, int flags)
{
  return ::open(path, flags);
}

int uuid_node_sys_read(int fd, void *buf, size_t count)
{
  const ssize_t n = ::read(fd, buf, count);
  if (n < 0) {
    return -1;
  }
  if (static_cast<size_t>(n) > count) {
    errno = EIO;
    return -1;
  }
  return static_cast<int>(n);
}

int uuid_node_sys_close(int fd)
{
  return ::close(fd);
}

const UuidCsprngOps UUID_NODE_REAL_CSPRNG = {
  uuid_node_sys_open,
  uuid_node_sys_read,
  uuid_node_sys_close,
};

// 0 empty, 1 fill in progress, 2 cache published with release/acquire.
std::atomic<int> g_uuid_node_cache_state{0};
unsigned char g_uuid_node_cache[UUID_NODE_LEN];

void uuid_node_after_fork_child()
{
  // fork copies only the calling thread. If that thread was not the filler,
  // state 1 would spin forever in the child. Drop the in-progress flag so the
  // child reads its own node. A published node stays in place: daemon() forks
  // after the static server_uuid initializer, and the child must keep that id.
  if (g_uuid_node_cache_state.load(std::memory_order_relaxed) == 1) {
    g_uuid_node_cache_state.store(0, std::memory_order_relaxed);
  }
}

#if !defined(__EMSCRIPTEN__)
int uuid_node_register_fork_handler()
{
  (void)pthread_atfork(nullptr, nullptr, uuid_node_after_fork_child);
  return 0;
}
int g_uuid_node_fork_handler = uuid_node_register_fork_handler();
#endif

#endif // !_WIN32

} // namespace

bool uuid_node_mac_usable(const unsigned char *mac)
{
  if (mac == nullptr) {
    return false;
  }
  for (int i = 0; i < UUID_NODE_LEN; ++i) {
    if (mac[i] != 0) {
      return true;
    }
  }
  return false;
}

static int read_csprng_node(unsigned char *node, const UuidCsprngOps *ops)
{
#if defined(_WIN32)
  (void)node;
  (void)ops;
  return uuid_node_unsupported();
#else
  if (node == nullptr) {
    return EINVAL;
  }
  if (ops == nullptr) {
    ops = &UUID_NODE_REAL_CSPRNG;
  }
  if (ops->open_fn == nullptr || ops->read_fn == nullptr || ops->close_fn == nullptr) {
    return EINVAL;
  }

  int fd = -1;
  int interrupts = 0;
  for (;;) {
    fd = ops->open_fn("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      break;
    }
    const int err = errno;
    if (err != EINTR || interrupts >= UUID_NODE_IO_RETRY_LIMIT) {
      return err != 0 ? err : EIO;
    }
    ++interrupts;
  }

  unsigned char tmp[UUID_NODE_LEN];
  size_t filled = 0;
  int err = 0;
  interrupts = 0;
  while (filled < static_cast<size_t>(UUID_NODE_LEN)) {
    const int n = ops->read_fn(fd, tmp + filled, static_cast<size_t>(UUID_NODE_LEN) - filled);
    if (n < 0) {
      const int saved = errno;
      if (saved == EINTR && interrupts < UUID_NODE_IO_RETRY_LIMIT) {
        ++interrupts;
        continue;
      }
      err = saved != 0 ? saved : EIO;
      break;
    }
    interrupts = 0;
    if (n == 0) {
      err = EIO;
      break;
    }
    if (static_cast<size_t>(n) > static_cast<size_t>(UUID_NODE_LEN) - filled) {
      err = EIO;
      break;
    }
    filled += static_cast<size_t>(n);
  }
  if (fd >= 0 && ops->close_fn != nullptr) {
    (void)ops->close_fn(fd);
  }
  if (err != 0) {
    return err;
  }
  // RFC 9562 §6.10: a node that is not an IEEE MAC sets the multicast bit.
  tmp[0] = static_cast<unsigned char>(tmp[0] | 0x01);
  memcpy(node, tmp, UUID_NODE_LEN);
  return 0;
#endif
}

int fill_random_uuid_node(unsigned char *node, const UuidCsprngOps *ops)
{
  if (node == nullptr) {
    return EINVAL;
  }
  // Injected ops stay uncached so tests can simulate partial reads and errors.
  if (ops != nullptr) {
    return read_csprng_node(node, ops);
  }
#if defined(_WIN32)
  return uuid_node_unsupported();
#else
#if !defined(__EMSCRIPTEN__)
  (void)g_uuid_node_fork_handler;
#endif
  for (;;) {
    int observed = g_uuid_node_cache_state.load(std::memory_order_acquire);
    if (observed == 2) {
      memcpy(node, g_uuid_node_cache, UUID_NODE_LEN);
      return 0;
    }
    if (observed == 0 &&
        g_uuid_node_cache_state.compare_exchange_weak(
            observed, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
      unsigned char tmp[UUID_NODE_LEN];
      const int rc = read_csprng_node(tmp, nullptr);
      if (rc != 0) {
        g_uuid_node_cache_state.store(0, std::memory_order_release);
        return rc;
      }
      memcpy(g_uuid_node_cache, tmp, UUID_NODE_LEN);
      g_uuid_node_cache_state.store(2, std::memory_order_release);
      memcpy(node, tmp, UUID_NODE_LEN);
      return 0;
    }
    std::this_thread::yield();
  }
#endif
}

int finish_uuid_node(bool mac_found,
                     const unsigned char *mac,
                     unsigned char *out,
                     bool *used_random,
                     const UuidCsprngOps *ops)
{
  if (out == nullptr || used_random == nullptr) {
    return EINVAL;
  }
  if (mac_found && mac == nullptr) {
    return EINVAL;
  }
  if (mac_found && uuid_node_mac_usable(mac)) {
    memmove(out, mac, UUID_NODE_LEN);
    *used_random = false;
    return 0;
  }
  const int rc = fill_random_uuid_node(out, ops);
  if (rc != 0) {
    return rc;
  }
  *used_random = true;
  return 0;
}

#if defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)

static void uuid_node_note_if_error(UuidMacDiscovery *out, const char *name, int err)
{
  out->hw_errno = err;
  memset(out->failed_ifname, 0, sizeof(out->failed_ifname));
  if (name == nullptr) {
    return;
  }
  for (int i = 0; i < UUID_IF_NAME_LEN - 1 && name[i] != '\0'; ++i) {
    out->failed_ifname[i] = name[i];
  }
}

static int uuid_node_sys_socket(int domain, int type, int protocol)
{
  return ::socket(domain, type, protocol);
}

static int uuid_node_sys_ioctl(int fd, unsigned long request, void *arg)
{
  return ::ioctl(fd, request, arg);
}

#endif

int discover_linux_mac(UuidMacDiscovery *out, const UuidMacOps *mac_ops)
{
  if (out == nullptr) {
    return EINVAL;
  }
  memset(out, 0, sizeof(*out));
#if !defined(__linux__) || defined(__ANDROID__) || defined(__EMSCRIPTEN__)
  (void)mac_ops;
  return uuid_node_unsupported();
#else
  static_assert(IFNAMSIZ <= UUID_IF_NAME_LEN, "UUID interface name field is too small");
  UuidMacOps real_ops;
  const UuidMacOps *ops = mac_ops;
  if (ops == nullptr) {
    real_ops.socket_fn = uuid_node_sys_socket;
    real_ops.ioctl_fn = uuid_node_sys_ioctl;
    real_ops.close_fn = uuid_node_sys_close;
    ops = &real_ops;
  }
  if (ops->socket_fn == nullptr || ops->ioctl_fn == nullptr || ops->close_fn == nullptr) {
    return EINVAL;
  }

  const int sock = ops->socket_fn(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    out->discovery_errno = errno;
    return 0;
  }

  struct ifreq reqs[128];
  struct ifconf ifc;
  memset(reqs, 0, sizeof(reqs));
  memset(&ifc, 0, sizeof(ifc));
  ifc.ifc_len = static_cast<int>(sizeof(reqs));
  ifc.ifc_buf = reinterpret_cast<char *>(reqs);
  if (ops->ioctl_fn(sock, SIOCGIFCONF, &ifc) < 0) {
    out->discovery_errno = errno;
    (void)ops->close_fn(sock);
    return 0;
  }

  int len = ifc.ifc_len;
  if (len < 0) {
    out->discovery_errno = EIO;
    (void)ops->close_fn(sock);
    return 0;
  }
  if (len > static_cast<int>(sizeof(reqs))) {
    len = static_cast<int>(sizeof(reqs));
  }
  const int count = len / static_cast<int>(sizeof(struct ifreq));
  for (int i = 0; i < count && !out->found; ++i) {
    char name[IFNAMSIZ];
    memset(name, 0, sizeof(name));
    memcpy(name, reqs[i].ifr_name, IFNAMSIZ - 1);
    if (name[0] == '\0') {
      continue;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ops->ioctl_fn(sock, SIOCGIFFLAGS, &ifr) < 0) {
      const int saved = errno;
      uuid_node_note_if_error(out, name, saved);
      continue;
    }
    const short flags = ifr.ifr_flags;
    if ((flags & IFF_LOOPBACK) != 0) {
      continue;
    }

    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ops->ioctl_fn(sock, SIOCGIFHWADDR, &ifr) < 0) {
      const int saved = errno;
      uuid_node_note_if_error(out, name, saved);
      continue;
    }
    unsigned char mac[UUID_NODE_LEN];
    memcpy(mac, ifr.ifr_hwaddr.sa_data, UUID_NODE_LEN);
    if (!uuid_node_mac_usable(mac)) {
      continue;
    }
    memcpy(out->mac, mac, UUID_NODE_LEN);
    out->found = true;
    out->hw_errno = 0;
    memset(out->failed_ifname, 0, sizeof(out->failed_ifname));
  }
  (void)ops->close_fn(sock);
  return 0;
#endif
}

int acquire_linux_uuid_node(unsigned char *out,
                            bool *used_random,
                            UuidMacDiscovery *discovery,
                            const UuidMacOps *mac_ops,
                            const UuidCsprngOps *rng_ops)
{
  if (out == nullptr || used_random == nullptr || discovery == nullptr) {
    return EINVAL;
  }
  const int discover_rc = discover_linux_mac(discovery, mac_ops);
  if (discover_rc != 0) {
    return discover_rc;
  }
  return finish_uuid_node(discovery->found, discovery->mac, out, used_random, rng_ops);
}

#ifdef OB_UUID_NODE_TEST
void uuid_node_test_reset_cache()
{
#if !defined(_WIN32)
  g_uuid_node_cache_state.store(0, std::memory_order_release);
  memset(g_uuid_node_cache, 0, sizeof(g_uuid_node_cache));
#endif
}
#endif

} // namespace sql
} // namespace oceanbase
