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

"""Focused production-method regression for UUID startup without a MAC address.

Run with Python 3.9+ and a C++17 compiler; no seekdb server or Google Test is
required. This runner extracts the actual production init, endian serialization,
UUID calc and formatting bodies unchanged, and compiles them in a temporary
directory. Linux uses real system interface headers. Windows models just the
Linux interface structures/constants. System calls, OpenSSL entropy and the
clock boundary are injected for deterministic fault coverage.

This validates the extracted control flow and UUID serialization, not full
seekdb linkage, actual sandbox policies, OpenSSL entropy, or clock concurrency.
It does not replace the full supported Linux integration build/test suite.

Examples:
  python unittest/sql/engine/expr/test_uuid_node_init.py
  python unittest/sql/engine/expr/test_uuid_node_init.py --source /path/to/old/ob_expr_uuid.cpp
  python unittest/sql/engine/expr/test_uuid_node_init.py --compiler clang++
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import uuid


SOURCE = Path(__file__).resolve().parents[4] / "src/sql/engine/expr/ob_expr_uuid.cpp"
COMPILER = os.environ.get("CXX")
TEMP_DIR = None
SIGNATURES = (
    "int ObUUIDNode::init()",
    "int ObBigEndian::put_uint16(unsigned char *b, uint16_t v)",
    "int ObBigEndian::put_uint32(unsigned char *b, uint32_t v)",
    "int UuidCommon::bin2uuid(char *result, const uchar *src)",
    "int ObExprUuid::calc(unsigned char *scratch)",
)
EXPECTED_CASES = (
    "real_mac_is_preserved",
    "socket_denied_uses_random",
    "ifconf_denied_uses_random",
    "flags_denied_uses_random",
    "hardware_denied_uses_random",
    "loopback_only_uses_random",
    "empty_list_uses_random",
    "flags_failure_continues_to_next_real_mac",
    "hardware_failure_continues_to_next_real_mac",
    "empty_list_uses_random_entropy_failure_is_reported",
    "socket_denied_uses_random_entropy_failure_is_reported",
    "ifconf_denied_uses_random_entropy_failure_is_reported",
    "random_node_sets_multicast_bit",
    "file_descriptor_zero_is_closed",
    "negative_entropy_result_is_rejected",
    "failed_partial_entropy_does_not_change_node",
    "random_node_reaches_uuid_v1_format",
    "repeated_uuid_keeps_node_without_new_entropy",
    "physical_mac_reaches_uuid_without_bit_changes",
)

def extract_function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise ValueError(f"Expected exactly one production function signature: {signature}")
    start = source.index(signature)
    position = source.index("{", start)
    depth = 0
    quote = None
    while position < len(source):
        character = source[position]
        if quote:
            if character == "\\":
                position += 2
                continue
            if character == quote:
                quote = None
        elif source.startswith("//", position):
            position = source.find("\n", position)
            if position < 0:
                break
            continue
        elif source.startswith("/*", position):
            comment_end = source.find("*/", position + 2)
            if comment_end < 0:
                raise ValueError(f"Unterminated comment inside {signature}")
            position = comment_end + 2
            continue
        elif character in ("\"", "'"):
            quote = character
        elif character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1] + "\n"
        position += 1
    raise ValueError(f"Unterminated production function: {signature}")

CPP_HARNESS = r"""
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define UUID_WINDOWS_HOST 1
#else
#define UUID_WINDOWS_HOST 0
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// The production method contains platform branches. Headers above are native;
// only the extracted method below is compiled through its Linux branch.
#undef _WIN32
#undef __APPLE__
#undef __ANDROID__

constexpr int OB_SUCCESS = 0;
constexpr int OB_ERR_UNEXPECTED = -4016;
constexpr int OB_ALLOCATE_MEMORY_FAILED = -4013;
#define OB_SUCC(value) ((value) == OB_SUCCESS)
#define OB_FAIL(value) ((ret = (value)) != OB_SUCCESS)
#define OB_ISNULL(value) ((value) == nullptr)
#define OB_UNLIKELY(value) (value)
#define MEMCPY std::memcpy
#define LOG_ERROR(...) do { ++scenario.log_calls; } while (0)
#define LOG_WARN(...) do { ++scenario.log_calls; } while (0)
#define LOG_INFO(...) do { ++scenario.log_calls; } while (0)

#if UUID_WINDOWS_HOST
constexpr int AF_INET = 2;
constexpr int SOCK_DGRAM = 2;
constexpr int IPPROTO_IP = 0;
constexpr int IFNAMSIZ = 16;
constexpr int IFF_LOOPBACK = 8;
constexpr unsigned long SIOCGIFCONF = 0x8912;
constexpr unsigned long SIOCGIFFLAGS = 0x8913;
constexpr unsigned long SIOCGIFHWADDR = 0x8927;

struct sockaddr {
  unsigned short sa_family;
  char sa_data[14];
};

struct ifreq {
  char ifr_name[IFNAMSIZ];
  union {
    short ifr_flags;
    sockaddr ifr_hwaddr;
  };
};

struct ifconf {
  int ifc_len;
  union {
    char *ifc_buf;
    ifreq *ifc_req;
  };
};
#endif

struct Interface {
  std::string name;
  bool loopback = false;
  bool flags_fail = false;
  bool hardware_fail = false;
  std::array<unsigned char, 6> mac{{0x02, 0x11, 0x22, 0x33, 0x44, 0x55}};
};

struct Scenario {
  std::string name;
  bool socket_fail = false;
  int socket_fd = 42;
  bool configuration_fail = false;
  bool entropy_fail = false;
  int entropy_result = 1;
  bool entropy_partial_write = false;
  std::vector<Interface> interfaces;
  std::array<unsigned char, 6> random{{0x20, 0x31, 0x42, 0x53, 0x64, 0x75}};
  bool expect_random = false;
  bool expect_success = true;
  int expected_flags_calls = 0;
  int expected_hardware_calls = 0;
  std::array<unsigned char, 6> expected_mac{{0x02, 0x11, 0x22, 0x33, 0x44, 0x55}};
  int socket_calls = 0;
  int close_calls = 0;
  int wrong_close_calls = 0;
  int random_calls = 0;
  int flags_calls = 0;
  int hardware_calls = 0;
  int stub_errors = 0;
  int log_calls = 0;
  std::vector<std::string> events;
};

Scenario scenario;

int stub_socket(int domain, int type, int protocol)
{
  ++scenario.socket_calls;
  scenario.events.push_back("socket");
  if (domain != AF_INET || type != SOCK_DGRAM || protocol != IPPROTO_IP) {
    ++scenario.stub_errors;
  }
  if (scenario.socket_fail) {
    errno = EACCES;
    return -1;
  }
  return scenario.socket_fd;
}

int stub_ioctl(int fd, unsigned long request, void *argument)
{
  if (fd != scenario.socket_fd) {
    ++scenario.stub_errors;
    return -1;
  }
  if (request == SIOCGIFCONF) {
    scenario.events.push_back("ifconf");
    if (scenario.configuration_fail) {
      errno = EACCES;
      return -1;
    }
    ifconf *configuration = static_cast<ifconf *>(argument);
    const std::size_t required = scenario.interfaces.size() * sizeof(ifreq);
    if (required > static_cast<std::size_t>(configuration->ifc_len)) {
      ++scenario.stub_errors;
      return -1;
    }
    for (std::size_t i = 0; i < scenario.interfaces.size(); ++i) {
      ifreq item{};
      std::strncpy(item.ifr_name, scenario.interfaces[i].name.c_str(), IFNAMSIZ - 1);
      std::memcpy(configuration->ifc_buf + i * sizeof(ifreq), &item, sizeof(item));
    }
    configuration->ifc_len = static_cast<int>(required);
    return 0;
  }

  ifreq *interface_request = static_cast<ifreq *>(argument);
  auto entry = std::find_if(scenario.interfaces.begin(), scenario.interfaces.end(),
      [interface_request](const Interface &interface) {
        return std::strncmp(interface_request->ifr_name,
            interface.name.c_str(), IFNAMSIZ - 1) == 0;
      });
  if (entry == scenario.interfaces.end()) {
    ++scenario.stub_errors;
    return -1;
  }
  if (request == SIOCGIFFLAGS) {
    ++scenario.flags_calls;
    scenario.events.push_back("flags:" + entry->name);
    if (entry->flags_fail) {
      errno = EACCES;
      return -1;
    }
    interface_request->ifr_flags = entry->loopback ? IFF_LOOPBACK : 0;
    return 0;
  }
  if (request == SIOCGIFHWADDR) {
    ++scenario.hardware_calls;
    scenario.events.push_back("hardware:" + entry->name);
    if (entry->hardware_fail) {
      errno = EACCES;
      return -1;
    }
    std::memcpy(interface_request->ifr_hwaddr.sa_data, entry->mac.data(), 6);
    return 0;
  }
  ++scenario.stub_errors;
  return -1;
}

int stub_close(int fd)
{
  scenario.events.push_back("close");
  ++scenario.close_calls;
  if (fd != scenario.socket_fd) {
    ++scenario.wrong_close_calls;
  }
  return 0;
}

int RAND_bytes(unsigned char *destination, int length)
{
  scenario.events.push_back("random");
  ++scenario.random_calls;
  if (length != 6 || destination == nullptr) {
    ++scenario.stub_errors;
    return 0;
  }
  if (scenario.entropy_fail) {
    if (scenario.entropy_partial_write) {
      std::memset(destination, 0x99, 3);
    }
    return scenario.entropy_result;
  }
  std::memcpy(destination, scenario.random.data(), 6);
  return 1;
}

// Matches the production method's data contract. The constructor deliberately
// is not modeled: each test invokes the actual extracted init() exactly once.
struct ObUUIDNode {
  int init();
  unsigned char mac_addr_[6];
  bool is_inited_;
};

using uchar = unsigned char;
struct ObBigEndian {
  static int put_uint16(unsigned char *bytes, uint16_t value);
  static int put_uint32(unsigned char *bytes, uint32_t value);
};
struct UuidCommon {
  static constexpr int LENGTH_UUID = 36;
  static int bin2uuid(char *result, const uchar *source);
};
uint64_t controlled_time = 0;
int time_calls = 0;
struct ObUUIDTime {
  static int get_time(uint64_t &time, uint16_t &sequence)
  {
    ++time_calls;
    time = controlled_time++;
    sequence = 0x9234;
    return OB_SUCCESS;
  }
};
struct ObExprUuid {
  static ObUUIDNode *uuid_node;
  static int calc(unsigned char *scratch);
};
ObUUIDNode *ObExprUuid::uuid_node = nullptr;

// Linux retains the real struct layouts/constants, but only these three calls
// are redirected. Windows supplies the minimal modeled Linux data structures.
#define socket stub_socket
#define ioctl stub_ioctl
#define close stub_close

#include "uuid_init.inc"
#include "uuid_additional.inc"

std::vector<Scenario> cases()
{
  std::vector<Scenario> result;
  Scenario real;
  real.name = "real_mac_is_preserved";
  real.interfaces = {Interface{"eth0"}};
  real.expected_flags_calls = 1;
  real.expected_hardware_calls = 1;
  result.push_back(real);

  Scenario denied;
  denied.name = "socket_denied_uses_random";
  denied.socket_fail = true;
  denied.expect_random = true;
  result.push_back(denied);

  Scenario configuration;
  configuration.name = "ifconf_denied_uses_random";
  configuration.configuration_fail = true;
  configuration.expect_random = true;
  result.push_back(configuration);

  Scenario flags;
  flags.name = "flags_denied_uses_random";
  Interface no_flags{"eth0"};
  no_flags.flags_fail = true;
  flags.interfaces = {no_flags};
  flags.expected_flags_calls = 1;
  flags.expect_random = true;
  result.push_back(flags);

  Scenario hardware;
  hardware.name = "hardware_denied_uses_random";
  Interface no_hardware{"eth0"};
  no_hardware.hardware_fail = true;
  hardware.interfaces = {no_hardware};
  hardware.expected_flags_calls = 1;
  hardware.expected_hardware_calls = 1;
  hardware.expect_random = true;
  result.push_back(hardware);

  Scenario loopback;
  loopback.name = "loopback_only_uses_random";
  Interface lo{"lo"};
  lo.loopback = true;
  loopback.interfaces = {lo};
  loopback.expected_flags_calls = 1;
  loopback.expect_random = true;
  result.push_back(loopback);

  Scenario empty;
  empty.name = "empty_list_uses_random";
  empty.expect_random = true;
  result.push_back(empty);

  Scenario later_after_flags = real;
  later_after_flags.name = "flags_failure_continues_to_next_real_mac";
  later_after_flags.interfaces = {no_flags, Interface{"eth1"}};
  later_after_flags.expected_flags_calls = 2;
  result.push_back(later_after_flags);

  Scenario later_after_hardware = real;
  later_after_hardware.name = "hardware_failure_continues_to_next_real_mac";
  later_after_hardware.interfaces = {no_hardware, Interface{"eth1"}};
  later_after_hardware.expected_flags_calls = 2;
  later_after_hardware.expected_hardware_calls = 2;
  result.push_back(later_after_hardware);

  for (Scenario entropy_source : {empty, denied, configuration}) {
    entropy_source.name += "_entropy_failure_is_reported";
    entropy_source.entropy_fail = true;
    entropy_source.entropy_result = 0;
    entropy_source.expect_success = false;
    result.push_back(entropy_source);
  }

  Scenario all_zero = empty;
  all_zero.name = "random_node_sets_multicast_bit";
  all_zero.random.fill(0);
  result.push_back(all_zero);

  Scenario descriptor_zero = empty;
  descriptor_zero.name = "file_descriptor_zero_is_closed";
  descriptor_zero.socket_fd = 0;
  result.push_back(descriptor_zero);

  Scenario negative_entropy = empty;
  negative_entropy.name = "negative_entropy_result_is_rejected";
  negative_entropy.entropy_fail = true;
  negative_entropy.entropy_result = -1;
  negative_entropy.expect_success = false;
  result.push_back(negative_entropy);

  Scenario partial_entropy = negative_entropy;
  partial_entropy.name = "failed_partial_entropy_does_not_change_node";
  partial_entropy.entropy_result = 0;
  partial_entropy.entropy_partial_write = true;
  result.push_back(partial_entropy);
  return result;
}

bool uuid_flow(bool physical_mac, bool check_format)
{
  scenario = Scenario{};
  if (physical_mac) {
    scenario.interfaces = {Interface{"eth0"}};
  }
  ObUUIDNode node{};
  const int initialized = node.init();
  ObExprUuid::uuid_node = &node;
  controlled_time = 0x0123456789abcdefULL;
  time_calls = 0;
  std::array<unsigned char, 16> first{};
  std::array<unsigned char, 16> second{};
  std::array<char, 37> text{};
  text.fill('!');
  const int first_result = ObExprUuid::calc(first.data());
  const int second_result = ObExprUuid::calc(second.data());
  const int format_result = UuidCommon::bin2uuid(text.data(), first.data());
  ObExprUuid::uuid_node = nullptr;
  bool passed = initialized == OB_SUCCESS && first_result == OB_SUCCESS
      && second_result == OB_SUCCESS && format_result == OB_SUCCESS;
  if (check_format) {
    const std::string expected = physical_mac
        ? "89abcdef-4567-1123-9234-021122334455"
        : "89abcdef-4567-1123-9234-213142536475";
    passed = passed && std::string(text.data(), 36) == expected
        && text[36] == '!' && ((first[6] >> 4) == 1)
        && ((first[8] & 0xc0) == 0x80);
  } else {
    passed = passed && first != second
        && std::equal(first.begin() + 10, first.end(), node.mac_addr_)
        && std::equal(second.begin() + 10, second.end(), node.mac_addr_)
        && scenario.random_calls == (physical_mac ? 0 : 1)
        && scenario.socket_calls == 1 && scenario.close_calls == 1
        && time_calls == 2;
  }
  return passed;
}

int main()
{
  int failed = 0;
  int total = 0;
  std::cout << "MODEL " << (UUID_WINDOWS_HOST ? "windows-linux-api-model" : "native-linux-headers") << '\n';
  for (const Scenario &test : cases()) {
    scenario = test;
    ObUUIDNode node{};
    // A failed reinitialization must not leave the previous initialized state.
    node.is_inited_ = true;
    std::memset(node.mac_addr_, 0xa5, sizeof(node.mac_addr_));
    const int code = node.init();
    const int expected_code = test.expect_success ? OB_SUCCESS : OB_ERR_UNEXPECTED;
    const int expected_closes = test.socket_fail ? 0 : 1;
    bool passed = code == expected_code && node.is_inited_ == test.expect_success
        && scenario.socket_calls == 1
        && scenario.close_calls == expected_closes
        && scenario.wrong_close_calls == 0
        && scenario.stub_errors == 0
        && scenario.random_calls == (test.expect_random ? 1 : 0)
        && scenario.flags_calls == test.expected_flags_calls
        && scenario.hardware_calls == test.expected_hardware_calls;

    if (test.expect_success) {
      auto expected_node = test.expect_random ? test.random : test.expected_mac;
      if (test.expect_random) {
        expected_node[0] |= 0x01;
      }
      passed = passed && std::equal(expected_node.begin(), expected_node.end(), node.mac_addr_);
    } else {
      passed = passed && std::all_of(std::begin(node.mac_addr_), std::end(node.mac_addr_),
          [](unsigned char value) { return value == 0xa5; });
    }
    if (test.expect_random && !test.socket_fail && scenario.random_calls != 0) {
      const auto close_event = std::find(scenario.events.begin(), scenario.events.end(), "close");
      const auto random_event = std::find(scenario.events.begin(), scenario.events.end(), "random");
      passed = passed && close_event != scenario.events.end() && close_event < random_event;
    }

    ++total;
    failed += passed ? 0 : 1;
    std::cout << (passed ? "PASS " : "FAIL ") << test.name
        << " ret=" << code << " initialized=" << node.is_inited_
        << " random_calls=" << scenario.random_calls
        << " close_calls=" << scenario.close_calls
        << " flags_calls=" << scenario.flags_calls
        << " hardware_calls=" << scenario.hardware_calls << '\n';
  }
  const auto check_flow = [&failed, &total](const char *name, bool physical_mac, bool check_format) {
    const bool passed = uuid_flow(physical_mac, check_format);
    ++total;
    failed += passed ? 0 : 1;
    std::cout << (passed ? "PASS " : "FAIL ") << name << '\n';
  };
  check_flow("random_node_reaches_uuid_v1_format", false, true);
  check_flow("repeated_uuid_keeps_node_without_new_entropy", false, false);
  check_flow("physical_mac_reaches_uuid_without_bit_changes", true, true);
  std::cout << "RESULT total=" << total << " passed=" << total - failed
      << " failed=" << failed << '\n';
  return failed == 0 ? 0 : 1;
}
"""


def windows_compiler_environment(folder: Path) -> tuple[dict[str, str], str]:
    env = {key.upper(): value for key, value in os.environ.items()}
    available = COMPILER or shutil.which("cl")
    if available and env.get("INCLUDE") and env.get("LIB"):
        return env, str(available)
    vswhere = (Path(env.get("PROGRAMFILES(X86)", r"C:\Program Files (x86)"))
               / "Microsoft Visual Studio/Installer/vswhere.exe")
    if not vswhere.is_file():
        raise RuntimeError("MSVC environment unavailable; run from a VS C++ developer prompt")
    installation = subprocess.run(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
         "installationPath", "-utf8"], capture_output=True, check=True,
    ).stdout.decode("utf-8").strip()
    if not installation:
        raise RuntimeError("Visual Studio C++ tools were not found")
    vcvars = Path(installation) / "VC/Auxiliary/Build/vcvars64.bat"
    script = folder / "load-msvc-env.cmd"
    script.write_text('@echo off\r\nchcp 65001 >nul\r\n'
                      f'call "{vcvars}" >nul\r\n'
                      'if errorlevel 1 exit /b 1\r\nset\r\n', encoding="utf-8")
    result = subprocess.run(
        ["cmd.exe", "/d", "/u", "/c", str(script)], capture_output=True,
    )
    if result.returncode:
        raise RuntimeError("Cannot load the MSVC environment: "
                           + result.stderr.decode("utf-16le", errors="replace"))
    for line in result.stdout.decode("utf-16le").splitlines():
        key, separator, value = line.partition("=")
        if separator and key:
            env[key.upper()] = value
    compiler = str(available) if available else shutil.which("cl", path=env.get("PATH"))
    if not compiler:
        raise RuntimeError("cl.exe was not found after loading the MSVC environment")
    return env, compiler


def build_and_run(folder: Path) -> tuple[dict[str, str], str]:
    source = SOURCE.read_text(encoding="utf-8")
    methods = [extract_function(source, signature) for signature in SIGNATURES]
    (folder / "uuid_init.inc").write_text(methods[0], encoding="utf-8")
    (folder / "uuid_additional.inc").write_text("\n".join(methods[1:]), encoding="utf-8")
    fixture = folder / "uuid_fixture.cpp"
    fixture.write_text(CPP_HARNESS, encoding="utf-8")
    explicit_msvc = COMPILER and Path(str(COMPILER)).name.lower() in ("cl", "cl.exe")
    msvc = os.name == "nt" and (COMPILER is None or explicit_msvc)
    executable = folder / ("uuid_fixture.exe" if os.name == "nt" else "uuid_fixture")
    if msvc:
        env, compiler = windows_compiler_environment(folder)
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/Od",
                   "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(folder), str(fixture),
                   "/Fe:" + str(executable), "/Fo:" + str(folder / "uuid_fixture.obj")]
    else:
        env = dict(os.environ)
        compiler = COMPILER or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise RuntimeError("A C++17 compiler is required; pass --compiler or add one to PATH")
        command = [str(compiler), "-std=c++17", "-Wall", "-Wextra", "-O0",
                   "-I", str(folder), str(fixture), "-o", str(executable)]
    env["TEMP"] = str(folder)
    env["TMP"] = str(folder)
    compiled = subprocess.run(command, cwd=folder, env=env, capture_output=True)
    if compiled.returncode:
        raise RuntimeError("Cannot compile the extracted production methods:\n"
                           + (compiled.stdout + compiled.stderr).decode("utf-8", errors="replace"))
    completed = subprocess.run([str(executable)], cwd=folder, env=env, capture_output=True)
    output = (completed.stdout + completed.stderr).decode("utf-8", errors="replace")
    findings = re.findall(r"^(PASS|FAIL) ([a-z0-9_]+)(?:\s.*)?$", output, flags=re.MULTILINE)
    results = {name: status for status, name in findings}
    if len(findings) != len(EXPECTED_CASES) or set(results) != set(EXPECTED_CASES):
        raise RuntimeError("The fixture did not execute exactly the expected cases:\n" + output)
    failed = sum(status == "FAIL" for status in results.values())
    if completed.returncode != (1 if failed else 0):
        raise RuntimeError("Unexpected fixture process exit:\n" + output)
    digest = hashlib.sha256(methods[0].encode()).hexdigest()
    details = f"source={SOURCE}\ninit_sha256={digest}\ncompiler={compiler}\n" + output
    return results, details


class UUIDNodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = (TEMP_DIR or Path(tempfile.gettempdir())).resolve()
        cls.scratch = parent / ("seekdb-uuid-node-" + uuid.uuid4().hex)
        # Inherit the parent ACL: Python's Windows mode=0o700 temporary
        # directories can exclude the restricted token of a sandboxed runner.
        cls.scratch.mkdir()
        cls.addClassCleanup(clean_scratch, cls.scratch, parent)
        cls.results, cls.details = build_and_run(cls.scratch)
        print(cls.details, flush=True)


def clean_scratch(folder: Path, parent: Path) -> None:
    resolved = folder.resolve()
    if resolved.parent != parent.resolve() or not resolved.name.startswith("seekdb-uuid-node-"):
        raise RuntimeError("Refusing to clean a directory outside the temporary build parent")
    shutil.rmtree(resolved)


def make_case(name):
    def test(self):
        self.assertEqual(self.results[name], "PASS", self.details)
    test.__name__ = "test_" + name
    return test


for case_name in EXPECTED_CASES:
    setattr(UUIDNodeTests, "test_" + case_name, make_case(case_name))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--compiler", default=COMPILER,
                        help="C++ compiler executable; defaults to CXX or auto-detection")
    parser.add_argument("--temp-dir", type=Path, help="Parent directory for temporary build files")
    options, unittest_options = parser.parse_known_args()
    SOURCE = options.source
    COMPILER = options.compiler
    TEMP_DIR = options.temp_dir.resolve() if options.temp_dir else None
    unittest.main(argv=[__file__, *unittest_options], verbosity=2)
