#!/usr/bin/env python3
"""Compile the real ObUUIDNode::init body with injected Linux syscalls."""
import pathlib
import subprocess
import sys
import tempfile

source_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2] / "src/sql/engine/expr/ob_expr_uuid.cpp"
source = source_path.read_text()
start = source.index("int ObUUIDNode::init()")
end = source.index("int ObUUIDTime::get_time", start)
function = source[start:end]

prefix = r'''
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <vector>

constexpr int OB_SUCCESS = 0;
constexpr int OB_ERR_UNEXPECTED = -4016;
#define OB_SUCC(x) ((x) == OB_SUCCESS)
#define OB_FAIL(x) ((x) != OB_SUCCESS)
#define OB_ISNULL(x) ((x) == NULL)
#define MEMCPY std::memcpy
#define K(x) x

struct Log { std::string message; std::vector<std::string> args; };
std::vector<Log> logs;
std::string stringify(int value) { return std::to_string(value); }
std::string stringify(const char *value) { return value == NULL ? "NULL" : value; }
template<typename... Args> void record(const char *message, Args... args) {
  logs.push_back({message, {stringify(args)...}});
  errno = ENOENT;  // Logging must not change the reported syscall error.
}
#define LOG_WARN(...) record(__VA_ARGS__)
#define LOG_ERROR(...) record(__VA_ARGS__)

enum Scenario { IFCONF_FAIL, FLAGS_FAIL, HW_FAIL, EMPTY, LOOPBACK, LATER_SUCCESS, SUCCESS };
Scenario scenario;
int fake_socket(int, int, int) { return 17; }
int fake_close(int) { return 0; }
int fake_ioctl(int, unsigned long request, void *arg) {
  if (request == SIOCGIFCONF) {
    if (scenario == IFCONF_FAIL) { errno = EPERM; return -1; }
    auto *ifc = static_cast<ifconf *>(arg);
    auto *entries = ifc->ifc_req;
    if (scenario == EMPTY) { ifc->ifc_len = 0; return 0; }
    std::strcpy(entries[0].ifr_name, "eth0");
    ifc->ifc_len = sizeof(ifreq);
    if (scenario == LATER_SUCCESS) {
      std::strcpy(entries[1].ifr_name, "eth1");
      ifc->ifc_len += sizeof(ifreq);
    }
    return 0;
  }
  auto *ifr = static_cast<ifreq *>(arg);
  if (request == SIOCGIFFLAGS) {
    if (scenario == FLAGS_FAIL) { errno = EACCES; return -1; }
    ifr->ifr_flags = scenario == LOOPBACK ? IFF_LOOPBACK : 0;
    return 0;
  }
  if (request == SIOCGIFHWADDR) {
    if (scenario == HW_FAIL || (scenario == LATER_SUCCESS && std::strcmp(ifr->ifr_name, "eth0") == 0)) {
      errno = ENODEV; return -1;
    }
    const char mac[6] = {1, 2, 3, 4, 5, 6};
    std::memcpy(ifr->ifr_hwaddr.sa_data, mac, sizeof(mac));
    return 0;
  }
  std::abort();
}
#define socket fake_socket
#define ioctl fake_ioctl
#define close fake_close
struct ObUUIDNode { unsigned char mac_addr_[6]; bool is_inited_; int init(); };
'''

suffix = r'''
bool has_log(const char *message, const char *arg = NULL) {
  for (const auto &log : logs) {
    if (log.message == message) {
      if (arg == NULL) return true;
      for (const auto &value : log.args) if (value == arg) return true;
    }
  }
  return false;
}
void check(bool condition, const char *name) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}
void run(Scenario test, bool expect_success, const char *message, const char *arg = NULL) {
  scenario = test;
  logs.clear();
  errno = 0;
  ObUUIDNode node{};
  const int ret = node.init();
  check(ret == (expect_success ? OB_SUCCESS : OB_ERR_UNEXPECTED), "return code");
  check(node.is_inited_ == expect_success, "initialized flag");
  if (message != NULL) check(has_log(message, arg), message);
  if (expect_success) check(node.mac_addr_[0] == 1 && node.mac_addr_[5] == 6, "MAC bytes");
}
int main() {
  run(IFCONF_FAIL, false, "ioctl failed, SIOCGIFCONF", "1");
  run(FLAGS_FAIL, false, "ioctl failed, SIOCGIFFLAGS", "13");
  run(FLAGS_FAIL, false, "failed to get UUID MAC address after interface ioctl errors", "SIOCGIFFLAGS");
  run(HW_FAIL, false, "ioctl failed, SIOCGIFHWADDR", "19");
  run(HW_FAIL, false, "failed to get UUID MAC address after interface ioctl errors", "SIOCGIFHWADDR");
  run(EMPTY, false, "no usable non-loopback MAC address found for UUID node");
  run(LOOPBACK, false, "no usable non-loopback MAC address found for UUID node");
  run(LATER_SUCCESS, true, NULL);
  run(SUCCESS, true, NULL);
  std::puts("PASS: 9 injected UUID node diagnostic cases");
}
'''

with tempfile.TemporaryDirectory(prefix="uuid-diagnostic-") as temp:
    cpp = pathlib.Path(temp) / "test.cpp"
    executable = pathlib.Path(temp) / "test"
    cpp.write_text(prefix + function + suffix)
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
