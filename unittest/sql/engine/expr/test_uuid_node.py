#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Isolated Linux UUID-node regression; run with Python 3 and a C++ compiler.

Compile the actual init method with deterministic syscall stubs. This also
exercises the Linux failure paths on hosts without Linux network interfaces.
It does not replace the full server build or startup integration tests.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[4] / "src/sql/engine/expr/ob_expr_uuid.cpp"

HARNESS = r"""
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
using ssize_t = std::ptrdiff_t;
#else
#include <sys/types.h>
#endif
// Select the production Linux branch after including the host's C++ headers.
#undef _WIN32
#undef __APPLE__
#undef __ANDROID__
#undef __EMSCRIPTEN__
#define OB_SUCCESS 0
#define OB_ERR_UNEXPECTED -1
#define OB_SUCC(value) ((value) == OB_SUCCESS)
#define OB_FAIL(value) ((ret = (value)) != OB_SUCCESS)
#define OB_ISNULL(value) ((value) == nullptr)
#define MEMCPY std::memcpy
#define LOG_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define IFNAMSIZ 16
#define IFF_LOOPBACK 1
#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_IP 0
#define SIOCGIFCONF 1
#define SIOCGIFFLAGS 2
#define SIOCGIFHWADDR 3
#define O_RDONLY 0
#define O_CLOEXEC 1

struct ifreq {
  char ifr_name[IFNAMSIZ];
  int ifr_flags;
  struct { char sa_data[14]; } ifr_hwaddr;
};
struct ifconf {
  int ifc_len;
  union { char *ifc_buf; ifreq *ifc_req; };
};
class ObUUIDNode {
public:
  bool is_inited_ = true;
  unsigned char mac_addr_[6] = {};
  int init();
};

enum Scenario {
  SOCKET_FAILURE, CONFIG_FAILURE, NO_INTERFACES, REAL_MAC,
  RANDOM_OPEN_FAILURE, RANDOM_READ_FAILURE, RANDOM_EOF, PARTIAL_READ,
  INTERRUPTED_OPEN, INTERRUPTED_READ, HARDWARE_FAILURE, LOOPBACK_ONLY,
  FLAGS_FAILURE
};
static int scenario, open_calls, read_calls, random_bytes, socket_closes, random_closes;
static const int socket_fd = 17, random_fd = 23;

static int fake_socket(int, int, int)
{
  errno = EPERM;
  return scenario == SOCKET_FAILURE ? -1 : socket_fd;
}
static int fake_ioctl(int, int command, void *argument)
{
  if (command == SIOCGIFCONF) {
    if (scenario == CONFIG_FAILURE) { errno = EPERM; return -1; }
    ifconf *config = static_cast<ifconf *>(argument);
    bool has_interface = scenario == REAL_MAC || scenario >= HARDWARE_FAILURE;
    config->ifc_len = has_interface ? sizeof(ifreq) : 0;
    if (has_interface) {
      ifreq interface = {};
      std::strcpy(interface.ifr_name, "test0");
      std::memcpy(config->ifc_buf, &interface, sizeof(interface));
    }
  } else if (command == SIOCGIFFLAGS) {
    if (scenario == FLAGS_FAILURE) { errno = EPERM; return -1; }
    static_cast<ifreq *>(argument)->ifr_flags = scenario == LOOPBACK_ONLY ? IFF_LOOPBACK : 0;
  } else if (command == SIOCGIFHWADDR) {
    if (scenario == HARDWARE_FAILURE) { errno = EPERM; return -1; }
    const unsigned char mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::memcpy(static_cast<ifreq *>(argument)->ifr_hwaddr.sa_data, mac, sizeof(mac));
  }
  return 0;
}
static int fake_open(const char *path, int)
{
  ++open_calls;
  if (std::strcmp(path, "/dev/urandom") != 0) std::abort();
  if (scenario == RANDOM_OPEN_FAILURE) { errno = EACCES; return -1; }
  if (scenario == INTERRUPTED_OPEN && open_calls == 1) { errno = EINTR; return -1; }
  return random_fd;
}
static ssize_t fake_read(int fd, void *buffer, size_t count)
{
  ++read_calls;
  if (fd != random_fd) std::abort();
  if (scenario == RANDOM_READ_FAILURE) { errno = EIO; return -1; }
  if (scenario == RANDOM_EOF) return 0;
  if (scenario == INTERRUPTED_READ && read_calls == 1) { errno = EINTR; return -1; }
  if (scenario == PARTIAL_READ && read_calls == 1) count = 2;
  for (size_t i = 0; i < count; ++i) {
    static_cast<unsigned char *>(buffer)[i] = static_cast<unsigned char>(0x20 + random_bytes++);
  }
  return static_cast<ssize_t>(count);
}
static int fake_close(int fd)
{
  if (fd == socket_fd) ++socket_closes;
  else if (fd == random_fd) ++random_closes;
  else std::abort();
  return 0;
}
#define socket fake_socket
#define ioctl fake_ioctl
#define open fake_open
#define read fake_read
#define close fake_close

// The complete production method is inserted here without rewriting its body.
@INIT_METHOD@

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (0)
int main(int argc, char **argv)
{
  if (argc != 2) return 2;
  scenario = std::atoi(argv[1]);
  ObUUIDNode node;
  int result = node.init();
  bool random_failure = scenario >= RANDOM_OPEN_FAILURE && scenario <= RANDOM_EOF;
  CHECK(socket_closes == (scenario == SOCKET_FAILURE ? 0 : 1));
  if (scenario == REAL_MAC) {
    const unsigned char mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    CHECK(result == OB_SUCCESS && node.is_inited_);
    CHECK(std::memcmp(node.mac_addr_, mac, sizeof(mac)) == 0);
    CHECK(open_calls == 0 && read_calls == 0 && random_closes == 0);
  } else {
    CHECK(open_calls == (scenario == INTERRUPTED_OPEN ? 2 : 1));
    CHECK(random_closes == (scenario == RANDOM_OPEN_FAILURE ? 0 : 1));
    if (random_failure) {
      CHECK(result != OB_SUCCESS && !node.is_inited_);
    } else {
      const unsigned char expected[6] = {0x21, 0x21, 0x22, 0x23, 0x24, 0x25};
      CHECK(result == OB_SUCCESS && node.is_inited_);
      CHECK(std::memcmp(node.mac_addr_, expected, sizeof(expected)) == 0);
      CHECK(random_bytes == 6);
      CHECK(read_calls == (scenario == PARTIAL_READ || scenario == INTERRUPTED_READ ? 2 : 1));
    }
  }
  return 0;
}
"""


class TestUuidNode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CXX") or next(
            (path for name in ("c++", "clang++", "g++", "cl") if (path := shutil.which(name))), None
        )
        if not compiler:
            raise unittest.SkipTest("A C++ compiler is required; set CXX or add one to PATH")
        source = SOURCE.read_text(encoding="utf-8")
        start = source.index("int ObUUIDNode::init()")
        end = source.index("\nint ObUUIDTime::get_time", start)
        cls.directory = tempfile.TemporaryDirectory(prefix="seekdb_uuid_node_")
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        cpp = directory / "uuid_node.cpp"
        cpp.write_text(HARNESS.replace("@INIT_METHOD@", source[start:end]), encoding="utf-8")
        cls.binary = directory / ("uuid_node.exe" if os.name == "nt" else "uuid_node")
        if Path(compiler).name.lower() in ("cl", "cl.exe"):
            command = [compiler, "/nologo", "/EHsc", "/std:c++14", str(cpp), "/Fe:" + str(cls.binary)]
        else:
            command = [compiler, "-std=c++11", str(cpp), "-o", str(cls.binary)]
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def test_initialization_paths(self):
        scenarios = (
            "socket failure", "interface enumeration failure", "no interfaces", "real MAC retained",
            "random source open failure", "random source read failure", "random source EOF",
            "partial random read", "interrupted random open", "interrupted random read",
            "hardware address unavailable", "loopback only", "interface flags unavailable",
        )
        for number, scenario in enumerate(scenarios):
            with self.subTest(scenario=scenario):
                result = subprocess.run(
                    [str(self.binary), str(number)], capture_output=True, text=True, timeout=10
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
