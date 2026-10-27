"""Compile the production UUID node code with deterministic Linux syscall faults.

Run with: python3 unittest/sql/engine/expr/test_uuid_node.py -v
Only the node's existing declaration and implementation are extracted; the SQL
engine, logging, and external OS/entropy dependencies are outside this unit test.
This keeps fault injection out of the production API and needs no server build.
"""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[4]

HARNESS_PREFIX = r"""
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr int OB_SUCCESS = 0;
constexpr int OB_ERR_UNEXPECTED = -4016;
#define OB_ISNULL(value) ((value) == nullptr)
#define OB_SUCC(value) ((value) == OB_SUCCESS)
#define OB_FAIL(value) ((ret = (value)) != OB_SUCCESS)
#define MEMCPY memcpy
#define LOG_ERROR(...)
#define LOG_WARN(...)
#define LOG_INFO(...)

static const char *scenario;
static int closes = 0;
static int entropy_calls = 0;
static bool invalid_call = false;

static bool is(const char *name) { return strcmp(scenario, name) == 0; }

static int test_socket(int family, int type, int protocol)
{
  invalid_call |= family != AF_INET || type != SOCK_DGRAM || protocol != IPPROTO_IP;
  if (is("socket_denied")) { errno = EPERM; return -1; }
  return 42;
}

static int test_ioctl(int fd, unsigned long request, void *arg)
{
  invalid_call |= fd != 42;
  if (request == SIOCGIFCONF) {
    if (is("enumeration_denied")) { errno = EPERM; return -1; }
    auto *config = static_cast<ifconf *>(arg);
    if (is("no_interfaces")) { config->ifc_len = 0; return 0; }
    invalid_call |= config->ifc_len < static_cast<int>(sizeof(ifreq));
    config->ifc_len = sizeof(ifreq);
    memset(config->ifc_req, 0, sizeof(ifreq));
    strcpy(config->ifc_req->ifr_name, "eth0");
  } else if (request == SIOCGIFFLAGS) {
    if (is("flags_denied")) { errno = EPERM; return -1; }
    auto *interface = static_cast<ifreq *>(arg);
    invalid_call |= strcmp(interface->ifr_name, "eth0") != 0;
    interface->ifr_flags = is("loopback_only") ? IFF_LOOPBACK : 0;
  } else if (request == SIOCGIFHWADDR) {
    if (!is("normal")) { errno = EPERM; return -1; }
    const unsigned char mac[] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(static_cast<ifreq *>(arg)->ifr_hwaddr.sa_data, mac, sizeof(mac));
  } else {
    invalid_call = true;
    return -1;
  }
  return 0;
}

static int test_close(int fd)
{
  invalid_call |= fd != 42;
  ++closes;
  return 0;
}

[[maybe_unused]] static int test_rand_bytes(unsigned char *output, int length)
{
  ++entropy_calls;
  invalid_call |= length != 6;
  if (is("entropy_denied")) { return 0; }
  if (is("entropy_partial")) { output[0] = 0x22; return -1; }
  const unsigned char random[] = {0x22, 0x11, 0x22, 0x33, 0x44, 0x55};
  memcpy(output, random, sizeof(random));
  return 1;
}

#define socket test_socket
#define ioctl test_ioctl
#define close test_close
#define RAND_bytes test_rand_bytes
"""

HARNESS_SUFFIX = r"""
int main(int argc, char **argv)
{
  if (argc != 2) { return 2; }
  scenario = "normal";
  ObUUIDNode node;
  scenario = argv[1];
  closes = 0;
  entropy_calls = 0;
  const int result = node.init();
  printf("%d %d %d %d %d ", result, node.is_inited_, closes, entropy_calls, invalid_call);
  for (unsigned char byte : node.mac_addr_) { printf("%02x", byte); }
  puts("");
}
"""


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux UUID node path")
class TestUuidNode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        header = (ROOT / "src/sql/engine/expr/ob_expr_uuid.h").read_text()
        source = (ROOT / "src/sql/engine/expr/ob_expr_uuid.cpp").read_text()
        declaration_start = header.index("struct ObUUIDNode\n")
        declaration_end = header.index("\n};", declaration_start) + len("\n};")
        definition_start = source.index("ObUUIDNode::ObUUIDNode()")
        definition_end = source.index("int ObUUIDTime::get_time", definition_start)
        cls.temporary = tempfile.TemporaryDirectory(prefix="seekdb-uuid-node-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        translation_unit = directory / "uuid_node.cpp"
        translation_unit.write_text(
            HARNESS_PREFIX + header[declaration_start:declaration_end]
            + source[definition_start:definition_end] + HARNESS_SUFFIX
        )
        cls.binary = directory / "uuid_node"
        command = shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++17", "-Wall", "-Wextra", "-Werror",
            *shlex.split(os.environ.get("UUID_NODE_TEST_CXXFLAGS", "")),
            str(translation_unit), "-o", str(cls.binary),
        ]
        subprocess.run(command, check=True, capture_output=True, text=True)

    def run_case(self, scenario):
        result = subprocess.run(
            [str(self.binary), scenario], check=True, capture_output=True, text=True
        )
        status, initialized, closes, entropy_calls, invalid_call, node = result.stdout.split()
        self.assertEqual("0", invalid_call, result.stdout)
        return int(status), int(initialized), int(closes), int(entropy_calls), node

    def test_preserves_physical_mac_without_requesting_entropy(self):
        self.assertEqual((0, 1, 1, 0, "021122334455"), self.run_case("normal"))

    def test_socket_denied_uses_random_node_without_closing_invalid_socket(self):
        self.assertEqual((0, 1, 0, 1, "231122334455"), self.run_case("socket_denied"))

    def test_discovery_failures_use_multicast_random_node_and_close_socket(self):
        for scenario in (
            "enumeration_denied", "flags_denied", "mac_denied",
            "loopback_only", "no_interfaces",
        ):
            with self.subTest(scenario=scenario):
                self.assertEqual((0, 1, 1, 1, "231122334455"), self.run_case(scenario))

    def test_entropy_failure_does_not_leave_previous_node_initialized(self):
        for scenario in ("entropy_denied", "entropy_partial"):
            with self.subTest(scenario=scenario):
                status, initialized, closes, entropy_calls, _ = self.run_case(scenario)
                self.assertEqual((-4016, 0, 1, 1), (status, initialized, closes, entropy_calls))


if __name__ == "__main__":
    unittest.main()
