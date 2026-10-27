/* Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static __thread int discovery_failed;
static __thread int random_reads;

static int mode_is(const char *value)
{
  const char *mode = getenv("UUID_NODE_FAULT");
  return mode != NULL && strcmp(mode, value) == 0;
}

int socket(int domain, int type, int protocol)
{
  int (*next_socket)(int, int, int) = dlsym(RTLD_NEXT, "socket");
  if (mode_is("socket-eperm") && domain == AF_INET && type == SOCK_DGRAM) {
    discovery_failed = 1;
    fprintf(stderr, "UUID_NODE_TEST %s\n", getenv("UUID_NODE_FAULT"));
    errno = EPERM;
    return -1;
  }
  return next_socket(domain, type, protocol);
}

int ioctl(int fd, unsigned long request, ...)
{
  int (*next_ioctl)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
  va_list args;
  va_start(args, request);
  void *argument = va_arg(args, void *);
  va_end(args);
  if (request == SIOCGIFCONF && (mode_is("enum-eperm") || mode_is("rng-short-eintr")
      || mode_is("rng-eof") || mode_is("rng-error"))) {
    discovery_failed = 1;
    fprintf(stderr, "UUID_NODE_TEST %s\n", getenv("UUID_NODE_FAULT"));
    errno = EPERM;
    return -1;
  }
  int result = next_ioctl(fd, request, argument);
  if (result == 0 && request == SIOCGIFHWADDR && mode_is("zero-mac")) {
    struct ifreq *interface = argument;
    memset(interface->ifr_hwaddr.sa_data, 0, 6);
  }
  return result;
}

ssize_t read(int fd, void *buffer, size_t size)
{
  ssize_t (*next_read)(int, void *, size_t) = dlsym(RTLD_NEXT, "read");
  char descriptor[64], target[128];
  snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
  ssize_t length = readlink(descriptor, target, sizeof(target) - 1);
  if (discovery_failed && length > 0) {
    target[length] = '\0';
    if (strcmp(target, "/dev/urandom") == 0) {
      ++random_reads;
      if (mode_is("rng-error")) {
        fprintf(stderr, "UUID_NODE_TEST entropy EIO\n");
        errno = EIO;
        return -1;
      }
      if (mode_is("rng-eof")) {
        if (random_reads > 1) fprintf(stderr, "UUID_NODE_TEST entropy EOF\n");
        return random_reads == 1 ? next_read(fd, buffer, size > 2 ? 2 : size) : 0;
      }
      if (mode_is("rng-short-eintr")) {
        if (random_reads == 1) {
          errno = EINTR;
          return -1;
        }
        if (size > 2) size = 2;
      }
    }
  }
  return next_read(fd, buffer, size);
}
