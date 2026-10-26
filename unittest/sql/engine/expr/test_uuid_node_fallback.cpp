/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/expr/uuid_node_fallback.h"
#include <cassert>
#include <iostream>
using oceanbase::sql::make_random_uuid_node;
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
  std::cout << "PASS UUID fallback: multicast bit, entropy preservation, atomic failure, zero entropy bytes\n";
}
