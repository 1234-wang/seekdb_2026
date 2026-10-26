/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstddef>
#include <cstring>

namespace oceanbase { namespace sql {
// RFC 9562 section 6.10: random 48-bit node IDs use the multicast bit to
// distinguish them from IEEE 802 addresses. Publish only complete entropy.
// Entropy callback returns 1 only on success (the OpenSSL RAND_bytes contract).
template <typename Entropy>
bool make_random_uuid_node(unsigned char (&node)[6], Entropy entropy)
{
  unsigned char temporary[6];
  if (entropy(temporary, sizeof(temporary)) != 1) return false;
  temporary[0] |= 0x01;
  std::memcpy(node, temporary, sizeof(temporary));
  return true;
}
}} // namespace oceanbase::sql
