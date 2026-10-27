/* Copyright (c) 2026 张轩博. Licensed under the Apache License, Version 2.0. */
/* Test fixture for #1480; intercepts MAC address discovery and optional entropy access. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

int ioctl(int fd, unsigned long request, ...)
{
  va_list args;
  va_start(args, request);
  void *arg = va_arg(args, void *);
  va_end(args);
  if (request == SIOCGIFHWADDR) {
    errno = EPERM;
    return -1;
  }
  int (*real_ioctl)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
  return real_ioctl(fd, request, arg);
}

int open(const char *path, int flags, ...)
{
  int (*real_open)(const char *, int, ...) = dlsym(RTLD_NEXT, "open");
  if (getenv("TEST_UUID_BLOCK_RANDOM") && strcmp(path, "/dev/urandom") == 0) {
    errno = EACCES;
    return -1;
  }
  if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, int);
    va_end(args);
    return real_open(path, flags, mode);
  }
  return real_open(path, flags);
}
