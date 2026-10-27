/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstring>

namespace oceanbase { namespace sql {
// A short read or signal must not publish a partial node. The caller supplies
// temporary storage; EOF and errors other than EINTR fail closed.
template <typename Read>
bool read_uuid_entropy(unsigned char *out, size_t length, Read read_bytes)
{
  size_t offset = 0;
  while (offset < length) {
    const auto count = read_bytes(out + offset, length - offset);
    if (count > 0 && static_cast<size_t>(count) <= length - offset) {
      offset += static_cast<size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

// RFC 9562 section 6.10: random 48-bit node IDs use the multicast bit to
// distinguish them from IEEE 802 addresses. Publish only complete entropy.
// Entropy callback returns 1 only after filling all requested bytes.
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
