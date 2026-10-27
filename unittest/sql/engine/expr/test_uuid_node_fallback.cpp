/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/expr/uuid_node_fallback.h"
#include <cassert>
#include <iostream>
using oceanbase::sql::make_random_uuid_node;
using oceanbase::sql::read_uuid_entropy;
int main()
{
  unsigned char node[6] = {2, 2, 2, 2, 2, 2};
  bool ok = make_random_uuid_node(node, [](unsigned char *out, size_t length) {
    assert(length == 6);
    for (size_t i = 0; i < length; ++i) out[i] = static_cast<unsigned char>(i * 2);
    return 1;
  });
  assert(ok && node[0] == 1 && node[1] == 2 && node[5] == 10);
  const unsigned char previous[6] = {1, 2, 4, 6, 8, 10};
  ok = make_random_uuid_node(node, [](unsigned char *out, size_t) { out[0] = 0xff; return 0; });
  assert(!ok && std::memcmp(node, previous, sizeof(node)) == 0);
  ok = make_random_uuid_node(node, [](unsigned char *, size_t) { return -1; });
  assert(!ok && std::memcmp(node, previous, sizeof(node)) == 0);
  ok = make_random_uuid_node(node, [](unsigned char *out, size_t n) { std::memset(out, 0, n); return 1; });
  assert(ok && node[0] == 1 && node[5] == 0);
  unsigned char bytes[6] = {};
  int reads = 0;
  assert(read_uuid_entropy(bytes, sizeof(bytes), [&reads](unsigned char *out, size_t n) -> long {
    ++reads;
    if (reads == 1) { errno = EINTR; return -1; }
    const size_t count = n > 2 ? 2 : n;
    std::memset(out, reads, count);
    return static_cast<long>(count);
  }));
  assert(reads == 4 && bytes[0] == 2 && bytes[2] == 3 && bytes[4] == 4);
  unsigned char before_failure[6];
  std::memcpy(before_failure, node, sizeof(node));
  for (bool eof : {false, true}) {
    reads = 0;
    ok = make_random_uuid_node(node, [&](unsigned char *out, size_t length) {
      return read_uuid_entropy(out, length, [&](unsigned char *buffer, size_t) -> long {
        if (++reads == 1) { buffer[0] = 0xfe; return 1; }
        errno = EIO;
        return eof ? 0 : -1;
      }) ? 1 : 0;
    });
    assert(!ok && reads == 2 && std::memcmp(node, before_failure, sizeof(node)) == 0);
  }
  std::cout << "PASS UUID fallback: marking, entropy preservation, atomic failure, zeros, short reads, EINTR, EOF, EIO\n";
}
