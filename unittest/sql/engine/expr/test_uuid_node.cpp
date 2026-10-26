/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Standalone regression driver, compiled only by test_uuid_node.py. The current
// competition snapshot has no unittest build files. This includes the actual
// ObUUIDNode declaration/definitions extracted by that runner, not a copied
// implementation. GNU ld --wrap injects syscall failures without production hooks.
#ifdef UUID_NODE_STANDALONE_TEST
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
  throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); \
} } while (false)

namespace uuid_node_test {
enum class Network { REAL, MAC, EMPTY, LOOPBACK, SOCKET_ERROR, CONF_ERROR,
                     FLAGS_ERROR, HW_ERROR, ZERO_MAC, NON_ETHERNET, MULTICAST,
                     SECOND_MAC, LONG_NAME };
enum class Entropy { OK, OPEN_ERROR, OPEN_EINTR, READ_ERROR, READ_EOF, SHORT_EINTR };
struct State {
  Network network = Network::MAC;
  Entropy entropy = Entropy::OK;
  int sockets = 0;
  int socket_closes = 0;
  int opens = 0;
  int random_closes = 0;
  int reads = 0;
  size_t offset = 0;
  std::vector<std::string> logs;
} state;
constexpr int SOCKET_FD = 101;
constexpr int RANDOM_FD = 102;
const unsigned char MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
const unsigned char RANDOM[6] = {0x20, 0x43, 0x65, 0x87, 0xa9, 0xcb};

template <typename T> std::string field(const char *name, const T &value)
{
  std::ostringstream out;
  out << name << '=' << value;
  return out.str();
}
template <typename... Args> void log(const char *message, const Args &...args)
{
  std::ostringstream out;
  out << message;
  ((out << ' ' << args), ...);
  state.logs.push_back(out.str());
}
bool logged(const std::string &text)
{
  for (const auto &line : state.logs) {
    if (line.find(text) != std::string::npos) { return true; }
  }
  return false;
}
void reset(Network network, Entropy entropy = Entropy::OK)
{
  state = State{};
  state.network = network;
  state.entropy = entropy;
  errno = ENOENT; // A successful syscall must not make this stale errno a diagnosis.
}
} // namespace uuid_node_test

// The runner supplies only the two error constants and project logging/memory
// shims. All platform includes, node members and implementation come from source.
#include "uuid_node_under_test.inc"

extern "C" {
int __real_socket(int, int, int);
int __real_ioctl(int, unsigned long, ...);
int __real_open(const char *, int, ...);
ssize_t __real_read(int, void *, size_t);
int __real_close(int);

int __wrap_socket(int domain, int type, int protocol)
{
  using namespace uuid_node_test;
  if (state.network == Network::REAL) { return __real_socket(domain, type, protocol); }
  ++state.sockets;
  if (state.network == Network::SOCKET_ERROR) { errno = EPERM; return -1; }
  return SOCKET_FD;
}
int __wrap_ioctl(int fd, unsigned long request, ...)
{
  using namespace uuid_node_test;
  va_list args;
  va_start(args, request);
  void *arg = va_arg(args, void *);
  va_end(args);
  if (state.network == Network::REAL) { return __real_ioctl(fd, request, arg); }
  CHECK(fd == SOCKET_FD);
  if (request == SIOCGIFCONF) {
    if (state.network == Network::CONF_ERROR) { errno = EACCES; return -1; }
    auto &conf = *static_cast<struct ifconf *>(arg);
    const int count = state.network == Network::EMPTY ? 0 : state.network == Network::SECOND_MAC ? 2 : 1;
    CHECK(conf.ifc_len >= static_cast<int>(count * sizeof(struct ifreq)));
    conf.ifc_len = count * sizeof(struct ifreq);
    for (int i = 0; i < count; ++i) {
      auto &entry = conf.ifc_req[i];
      std::memset(&entry, 0, sizeof(entry));
      if (state.network == Network::LONG_NAME) {
        std::memset(entry.ifr_name, 'x', IFNAMSIZ); // deliberately not terminated
      } else {
        std::snprintf(entry.ifr_name, IFNAMSIZ, "eth%d", i);
      }
    }
    return 0;
  }
  auto &ifr = *static_cast<struct ifreq *>(arg);
  CHECK(ifr.ifr_name[IFNAMSIZ - 1] == '\0');
  if (request == SIOCGIFFLAGS) {
    if (state.network == Network::FLAGS_ERROR) { errno = EPERM; return -1; }
    ifr.ifr_flags = state.network == Network::LOOPBACK ? IFF_LOOPBACK : IFF_UP;
  } else if (request == SIOCGIFHWADDR) {
    if (state.network == Network::HW_ERROR
        || (state.network == Network::SECOND_MAC && std::strcmp(ifr.ifr_name, "eth0") == 0)) {
      errno = ENODEV;
      return -1;
    }
    ifr.ifr_hwaddr.sa_family = state.network == Network::NON_ETHERNET ? ARPHRD_NONE : ARPHRD_ETHER;
    std::memcpy(ifr.ifr_hwaddr.sa_data, MAC, sizeof(MAC));
    if (state.network == Network::ZERO_MAC) { std::memset(ifr.ifr_hwaddr.sa_data, 0, sizeof(MAC)); }
    if (state.network == Network::MULTICAST) { ifr.ifr_hwaddr.sa_data[0] |= 1; }
  } else {
    CHECK(false);
  }
  return 0;
}
int __wrap_open(const char *path, int flags, ...)
{
  using namespace uuid_node_test;
  ++state.opens;
  CHECK(std::strcmp(path, "/dev/urandom") == 0);
  CHECK((flags & O_ACCMODE) == O_RDONLY);
  CHECK((flags & O_CLOEXEC) != 0);
  if (state.network == Network::REAL) { return __real_open(path, flags); }
  if (state.entropy == Entropy::OPEN_ERROR) { errno = EACCES; return -1; }
  if (state.entropy == Entropy::OPEN_EINTR && state.opens == 1) { errno = EINTR; return -1; }
  return RANDOM_FD;
}
ssize_t __wrap_read(int fd, void *buffer, size_t count)
{
  using namespace uuid_node_test;
  if (state.network == Network::REAL) { return __real_read(fd, buffer, count); }
  CHECK(fd == RANDOM_FD);
  ++state.reads;
  if (state.entropy == Entropy::SHORT_EINTR && state.reads == 2) { errno = EINTR; return -1; }
  if (state.offset > 0 && state.entropy == Entropy::READ_ERROR) { errno = EIO; return -1; }
  if (state.offset > 0 && state.entropy == Entropy::READ_EOF) { errno = ENOENT; return 0; }
  CHECK(count <= sizeof(RANDOM) - state.offset);
  count = state.entropy == Entropy::OK ? count : std::min(count, size_t(2));
  std::memcpy(buffer, RANDOM + state.offset, count);
  state.offset += count;
  return count;
}
ssize_t __wrap___read_chk(int fd, void *buffer, size_t count, size_t buffer_size)
{
  // glibc FORTIFY can redirect read() here, notably with ASan enabled.
  CHECK(count <= buffer_size);
  return __wrap_read(fd, buffer, count);
}
int __wrap_close(int fd)
{
  using namespace uuid_node_test;
  if (state.network == Network::REAL) { return __real_close(fd); }
  if (fd == SOCKET_FD) { ++state.socket_closes; }
  else if (fd == RANDOM_FD) { ++state.random_closes; }
  else { CHECK(false); }
  return 0;
}
} // extern "C"

using oceanbase::sql::ObUUIDNode;
using namespace uuid_node_test;

static void test_mock_cases()
{
  int cases = 0;
  for (auto network : {Network::MAC, Network::SECOND_MAC, Network::LONG_NAME}) {
    reset(network);
    ObUUIDNode node;
    CHECK(node.is_inited_);
    CHECK(std::memcmp(node.mac_addr_, MAC, sizeof(MAC)) == 0);
    CHECK(state.opens == 0 && state.socket_closes == 1);
    ++cases;
  }
  for (auto network : {Network::EMPTY, Network::LOOPBACK, Network::SOCKET_ERROR,
                       Network::CONF_ERROR, Network::FLAGS_ERROR, Network::HW_ERROR,
                       Network::ZERO_MAC, Network::NON_ETHERNET, Network::MULTICAST}) {
    reset(network);
    ObUUIDNode node;
    unsigned char expected[6];
    std::memcpy(expected, RANDOM, sizeof(expected));
    expected[0] |= 1;
    CHECK(node.is_inited_ && std::memcmp(node.mac_addr_, expected, sizeof(expected)) == 0);
    CHECK(state.opens == 1 && state.reads == 1 && state.random_closes == 1);
    CHECK(state.socket_closes == (network == Network::SOCKET_ERROR ? 0 : 1));
    if (network == Network::SOCKET_ERROR) { CHECK(logged("socket_errno=" + std::to_string(EPERM))); }
    if (network == Network::CONF_ERROR) { CHECK(logged("SIOCGIFCONF") && logged("ioctl_errno=" + std::to_string(EACCES))); }
    if (network == Network::FLAGS_ERROR) { CHECK(logged("SIOCGIFFLAGS") && logged("ioctl_errno=" + std::to_string(EPERM))); }
    if (network == Network::HW_ERROR) { CHECK(logged("SIOCGIFHWADDR") && logged("ioctl_errno=" + std::to_string(ENODEV))); }
    if (network == Network::EMPTY) { CHECK(!logged("errno=")); }
    ++cases;
  }
  for (auto entropy : {Entropy::OPEN_EINTR, Entropy::SHORT_EINTR}) {
    reset(Network::EMPTY, entropy);
    ObUUIDNode node;
    CHECK(node.is_inited_ && node.mac_addr_[0] == (RANDOM[0] | 1));
    CHECK(std::memcmp(node.mac_addr_ + 1, RANDOM + 1, 5) == 0);
    CHECK(state.random_closes == 1 && state.socket_closes == 1);
    CHECK(state.opens == (entropy == Entropy::OPEN_EINTR ? 2 : 1));
    if (entropy == Entropy::SHORT_EINTR) { CHECK(state.reads == 4); }
    ++cases;
  }
  for (auto entropy : {Entropy::OPEN_ERROR, Entropy::READ_ERROR, Entropy::READ_EOF}) {
    reset(Network::MAC);
    ObUUIDNode node;
    CHECK(node.is_inited_);
    reset(Network::EMPTY, entropy);
    CHECK(node.init() != 0);
    CHECK(!node.is_inited_);
    const unsigned char zero[6] = {};
    CHECK(std::memcmp(node.mac_addr_, zero, sizeof(zero)) == 0);
    CHECK(state.socket_closes == 1);
    CHECK(state.random_closes == (entropy == Entropy::OPEN_ERROR ? 0 : 1));
    if (entropy == Entropy::OPEN_ERROR) { CHECK(logged("open_errno=" + std::to_string(EACCES))); }
    if (entropy == Entropy::READ_ERROR) { CHECK(logged("read_errno=" + std::to_string(EIO))); }
    if (entropy == Entropy::READ_EOF) { CHECK(logged("EOF") && !logged("errno=")); }
    reset(Network::EMPTY);
    CHECK(node.init() == 0 && node.is_inited_); // a failed attempt remains retryable
    ++cases;
  }
  std::cout << "PASS " << cases << " injected syscall cases\n";
}

static void test_real(bool loopback, bool expect_failure)
{
  reset(Network::REAL);
  if (loopback) {
    struct if_nameindex *interfaces = if_nameindex();
    CHECK(interfaces != nullptr);
    int count = 0;
    for (auto *it = interfaces; it->if_index != 0; ++it) {
      CHECK(std::strcmp(it->if_name, "lo") == 0);
      ++count;
    }
    if_freenameindex(interfaces);
    CHECK(count == 1);
  }
  ObUUIDNode node;
  if (expect_failure) {
    CHECK(!node.is_inited_ && node.init() != 0);
    std::cout << "PASS baseline initialization fails in loopback-only namespace\n";
  } else {
    CHECK(node.is_inited_);
    if (loopback) { CHECK((node.mac_addr_[0] & 1) != 0 && state.opens == 1); }
    else { CHECK(state.opens == 0); } // this control requires a usable host MAC
    std::cout << "PASS " << (loopback ? "loopback-only random node" : "host hardware MAC") << ": ";
    for (unsigned char byte : node.mac_addr_) {
      std::cout << std::hex << std::setw(2) << std::setfill('0') << int(byte);
    }
    std::cout << '\n';
  }
}

int main(int argc, char **argv)
{
  try {
    const std::string mode = argc > 1 ? argv[1] : "mock";
    if (mode == "mock") { test_mock_cases(); }
    else if (mode == "real") { test_real(false, false); }
    else if (mode == "loopback") { test_real(true, false); }
    else if (mode == "baseline-loopback") { test_real(true, true); }
    else { CHECK(false); }
  } catch (const std::exception &error) {
    std::cerr << error.what() << " network=" << static_cast<int>(state.network)
              << " entropy=" << static_cast<int>(state.entropy)
              << " opens=" << state.opens << " reads=" << state.reads << '\n';
    for (const auto &line : state.logs) { std::cerr << line << '\n'; }
    return 1;
  }
  return 0;
}
#endif // UUID_NODE_STANDALONE_TEST
