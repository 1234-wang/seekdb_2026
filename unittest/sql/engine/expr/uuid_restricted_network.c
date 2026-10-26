/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// LD_PRELOAD fault injector for issue #1480. Restricts only MAC discovery;
// ordinary file I/O, TCP SQL and all other ioctl operations are passed through.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
int ioctl(int fd, unsigned long request, ...)
{
  va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
  if (request == SIOCGIFCONF) {
    const char *mode = getenv("SEEKDB_UUID_TEST_MODE");
    if (mode && strcmp(mode, "empty") == 0) { ((struct ifconf *)arg)->ifc_len = 0; return 0; }
    errno = EPERM; return -1;
  }
  int (*real_ioctl)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
  return real_ioctl(fd, request, arg);
}
