#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

static int entropy_fd = -1;
static int entropy_reads = 0;
static int entropy_open_interrupted = 0;

int open(const char *path, int flags, ...)
{
  static int (*real_open)(const char *, int, ...) = NULL;
  if (!real_open) {
    real_open = dlsym(RTLD_NEXT, "open");
  }
  mode_t creation_mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    creation_mode = va_arg(args, mode_t);
    va_end(args);
  }
  const char *mode = getenv("SEEKDB_RESTRICT_MAC");
  if (mode && strcmp(mode, "rng_short") == 0 &&
      strcmp(path, "/dev/urandom") == 0 && !entropy_open_interrupted) {
    entropy_open_interrupted = 1;
    errno = EINTR;
    return -1;
  }
  int fd = real_open(path, flags, creation_mode);
  if (fd >= 0 && strcmp(path, "/dev/urandom") == 0) {
    entropy_fd = fd;
    entropy_reads = 0;
  }
  return fd;
}

ssize_t read(int fd, void *buffer, size_t count)
{
  static ssize_t (*real_read)(int, void *, size_t) = NULL;
  if (!real_read) {
    real_read = dlsym(RTLD_NEXT, "read");
  }
  const char *mode = getenv("SEEKDB_RESTRICT_MAC");
  if (fd == entropy_fd && mode && strcmp(mode, "rng_short") == 0) {
    if (entropy_reads++ == 0) {
      errno = EINTR;
      return -1;
    }
    if (count > 2) {
      count = 2;
    }
  } else if (fd == entropy_fd && mode && strcmp(mode, "rng_fail") == 0) {
    if (entropy_reads++ > 0) {
      errno = EIO;
      return -1;
    }
    if (count > 2) {
      count = 2;
    }
  }
  return real_read(fd, buffer, count);
}

int close(int fd)
{
  static int (*real_close)(int) = NULL;
  if (!real_close) {
    real_close = dlsym(RTLD_NEXT, "close");
  }
  if (fd == entropy_fd) {
    entropy_fd = -1;
  }
  return real_close(fd);
}

/* Reproduce issue #1480 without changing any real interface or sandbox policy. */
int ioctl(int fd, unsigned long request, ...)
{
  va_list args;
  va_start(args, request);
  void *arg = va_arg(args, void *);
  va_end(args);
  const char *mode = getenv("SEEKDB_RESTRICT_MAC");
  if (mode && request == SIOCGIFCONF) {
    if (mode[0] == 'e' || mode[0] == 'r') {
      errno = EPERM;
      return -1;
    }
    if (mode[0] == 'n') {
      ((struct ifconf *)arg)->ifc_len = 0;
      return 0;
    }
  }
  if (mode && mode[0] == 'f' && request == SIOCGIFFLAGS) {
    errno = EPERM;
    return -1;
  }
  if (mode && mode[0] == 'h' && request == SIOCGIFHWADDR) {
    errno = EPERM;
    return -1;
  }
  static int (*real_ioctl)(int, unsigned long, ...) = NULL;
  if (!real_ioctl) {
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
  }
  return real_ioctl(fd, request, arg);
}
