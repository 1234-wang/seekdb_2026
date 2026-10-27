#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#ifdef __ANDROID__
#include <ifaddrs.h>
#include <linux/if_packet.h>
#endif

#define OB_SUCCESS 0
#define OB_ERR_UNEXPECTED -4016
#define OB_SUCC(value) ((value) == OB_SUCCESS)
#define OB_FAIL(value) ((ret = (value)) != OB_SUCCESS)
#define OB_ISNULL(value) ((value) == nullptr)
#define MEMCPY memcpy
#define MEMSET memset
#define K(value) (value)
template <typename... Args> void log_stub(Args...) {}
#define LOG_WARN(...) log_stub(__VA_ARGS__)
#define LOG_ERROR(...) log_stub(__VA_ARGS__)

enum Discovery { MAC, EMPTY, LOOPBACK, SOCKET_ERROR, ENUM_ERROR, FLAGS_ERROR, MAC_ERROR };
enum Entropy { NORMAL, INTERRUPTED, PARTIAL, PARTIAL_INTERRUPTED, OPEN_ERROR,
               READ_ERROR, END_OF_FILE, PARTIAL_ERROR };
static Discovery discovery = MAC;
static Entropy entropy = NORMAL;
static int opens = 0, reads = 0, closes = 0;
static const unsigned char hardware_node[6] = {2, 3, 4, 5, 6, 7};
#ifdef __ANDROID__
static int fake_getifaddrs(ifaddrs **result)
{
  static ifaddrs interface;
  static sockaddr_ll address;
  if (discovery == ENUM_ERROR) { errno = EPERM; return -1; }
  memset(&interface, 0, sizeof(interface));
  memset(&address, 0, sizeof(address));
  address.sll_family = AF_PACKET;
  address.sll_halen = 6;
  memcpy(address.sll_addr, hardware_node, 6);
  interface.ifa_addr = reinterpret_cast<sockaddr *>(&address);
  interface.ifa_flags = discovery == LOOPBACK ? IFF_LOOPBACK : 0;
  *result = discovery == EMPTY ? nullptr : &interface;
  return 0;
}
static void fake_freeifaddrs(ifaddrs *) {}
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs
#else
static int fake_socket(int, int, int)
{
  if (discovery == SOCKET_ERROR) { errno = EPERM; return -1; }
  return 100;
}
static int fake_ioctl(int, unsigned long request, void *argument)
{
  if (request == SIOCGIFCONF) {
    if (discovery == ENUM_ERROR) { errno = EPERM; return -1; }
    auto *config = static_cast<ifconf *>(argument);
    config->ifc_len = discovery == EMPTY ? 0 : sizeof(ifreq);
    if (config->ifc_len) {
      memset(config->ifc_req, 0, sizeof(ifreq));
      strcpy(config->ifc_req->ifr_name, "eth0");
    }
  } else if (request == SIOCGIFFLAGS) {
    if (discovery == FLAGS_ERROR) { errno = EPERM; return -1; }
    static_cast<ifreq *>(argument)->ifr_flags = discovery == LOOPBACK ? IFF_LOOPBACK : 0;
  } else if (request == SIOCGIFHWADDR) {
    if (discovery == MAC_ERROR) { errno = EPERM; return -1; }
    memcpy(static_cast<ifreq *>(argument)->ifr_hwaddr.sa_data, hardware_node, 6);
  } else { abort(); }
  return 0;
}
#endif
static int fake_open(const char *path, int flags)
{
  ++opens;
  if (entropy == OPEN_ERROR) { errno = EACCES; return -1; }
  if (entropy == INTERRUPTED && opens == 1) { errno = EINTR; return -1; }
  return open(path, flags);
}
static ssize_t fake_read(int fd, void *buffer, size_t size)
{
  ++reads;
  if (entropy == INTERRUPTED && reads == 1) { errno = EINTR; return -1; }
  if (entropy == PARTIAL_INTERRUPTED && reads == 2) { errno = EINTR; return -1; }
  if (entropy == READ_ERROR || (entropy == PARTIAL_ERROR && reads > 1)) {
    errno = EIO; return -1;
  }
  if (entropy == END_OF_FILE) { return 0; }
  if (entropy == PARTIAL || entropy == PARTIAL_ERROR || entropy == PARTIAL_INTERRUPTED) { size = 1; }
  return read(fd, buffer, size);
}
static int fake_close(int fd)
{
  ++closes;
  return fd == 100 ? 0 : close(fd);
}
#define socket fake_socket
#define ioctl fake_ioctl
#define open fake_open
#define read fake_read
#define close fake_close
struct ObUUIDNode {
  ObUUIDNode();
  int init();
  unsigned char mac_addr_[6];
  bool is_inited_;
};
// PRODUCTION_UUID_NODE_INITIALIZER
#undef socket
#undef ioctl
#undef open
#undef read
#undef close
#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "failed at line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)
int main()
{
  int cases = 0;
  for (auto mode : {
#ifdef __ANDROID__
      MAC, EMPTY, LOOPBACK, ENUM_ERROR
#else
      MAC, EMPTY, LOOPBACK, SOCKET_ERROR, ENUM_ERROR, FLAGS_ERROR, MAC_ERROR
#endif
      }) {
    discovery = mode;
    entropy = NORMAL;
    opens = reads = closes = 0;
    ObUUIDNode node;
    CHECK(node.is_inited_);
    if (mode == MAC) {
      CHECK(memcmp(node.mac_addr_, hardware_node, 6) == 0);
      CHECK(opens == 0 && reads == 0);
    } else {
      CHECK((node.mac_addr_[0] & 1) == 1);
      CHECK(opens == 1 && reads >= 1);
    }
#ifdef __ANDROID__
    CHECK(closes == (mode == MAC ? 0 : 1));
#else
    CHECK(closes == (mode == MAC ? 1 : mode == SOCKET_ERROR ? 1 : 2));
#endif
    ++cases;
  }
  discovery = EMPTY;
  for (auto mode : {INTERRUPTED, PARTIAL, PARTIAL_INTERRUPTED, OPEN_ERROR,
                    READ_ERROR, END_OF_FILE, PARTIAL_ERROR}) {
    entropy = mode;
    ObUUIDNode node;
    memset(node.mac_addr_, 0xA4, 6);
    opens = reads = closes = 0;
    int ret = node.init();
    bool success = mode == INTERRUPTED || mode == PARTIAL || mode == PARTIAL_INTERRUPTED;
    CHECK((ret == OB_SUCCESS) == success);
    CHECK(node.is_inited_ == success);
#ifdef __ANDROID__
    CHECK(closes == (mode == OPEN_ERROR ? 0 : 1));
#else
    CHECK(closes == (mode == OPEN_ERROR ? 1 : 2));
#endif
    if (success) {
      CHECK(node.mac_addr_[0] & 1);
      CHECK(reads > 1);
    } else {
      for (auto byte : node.mac_addr_) { CHECK(byte == 0xA4); }
    }
    ++cases;
  }
  entropy = NORMAL;
  ObUUIDNode first;
  ObUUIDNode second;
  CHECK(memcmp(first.mac_addr_, second.mac_addr_, 6) != 0);
  printf("PASS: %d UUID node fault-injection cases and independent random nodes (%s)\n", cases,
#ifdef __ANDROID__
         "Android branch"
#else
         "Linux branch"
#endif
         );
}
