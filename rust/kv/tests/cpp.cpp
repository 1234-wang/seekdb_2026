// Copyright (c) 2026 OceanBase.
// SPDX-License-Identifier: Apache-2.0

#include "kv.hpp"
#include <cassert>
#include <string>

int main(int argc, char **argv)
{
  assert(argc == 2);
  kv::Cursor retained_cursor;
  {
    kv::Store store;
    assert(kv::Store::open(argv[1], store));
    kv::Store duplicate;
    assert(kv::Store::open(argv[1], duplicate).value.code == KvCode_Busy);
    kv::CommitInfo info{};
    assert(store.write([](kv::Transaction &transaction) {
      kv::Status status = transaction.put("a", "old");
      if (!status) { return status; }
      status = transaction.put("b", "");
      if (!status) { return status; }
      return transaction.get("b", [](std::string_view value) {
        assert(value.empty());
        return kv::Status{};
      });
    }, &info));
    assert(info.bytes_written > 0);
    kv::Snapshot original;
    assert(store.snapshot(original));
    assert(original.scan(kv::all(), retained_cursor));
    assert(store.write([](kv::Transaction &transaction) {
      kv::Status status = transaction.put(std::string_view("a\0b", 3), std::string_view("v\0z", 3));
      if (!status) { return status; }
      return transaction.get("a", [&](std::string_view value) {
        assert(value == "old");
        return transaction.put("a", "new");
      });
    }));
    kv::Snapshot current;
    assert(store.snapshot(current));
    kv::Cursor prefix;
    assert(current.prefix(std::string_view("a\0", 2), prefix));
    assert(prefix.next([](std::string_view key, std::string_view value) {
      assert(key == std::string_view("a\0b", 3) && value == std::string_view("v\0z", 3));
      return kv::Status{};
    }));
    assert(prefix.next([](std::string_view, std::string_view) { return kv::Status{}; }).end());
    assert(original.get("a", [](std::string_view value) {
      assert(value == "old");
      return kv::Status{};
    }));
    assert(store.write([](kv::Transaction &transaction) {
      kv::Status status = transaction.put("a", "rollback");
      assert(status);
      return kv::Status{{KvCode_Aborted, 0}};
    }).value.code == KvCode_Aborted);
    assert(store.get("a", [](std::string_view value) {
      assert(value == "new");
      return kv::Status{};
    }));
    assert(store.get("absent", [](std::string_view) { return kv::Status{}; }).not_found());
    const std::string oversized(513, 'k');
    assert(store.write([&](kv::Transaction &transaction) {
      assert(transaction.put("a", "must-rollback"));
      assert(!transaction.put(oversized, "invalid"));
      // A failed mutation must prevent commit even if the callback ignores it.
      return kv::Status{};
    }).value.code == KvCode_Aborted);
    assert(store.write([](kv::Transaction &transaction) {
      bool found = false;
      kv::Status status = transaction.scan(kv::all(), [&](std::string_view key, std::string_view) {
        found = true;
        assert(key == "a");
        return kv::Status{{KvCode_End, 0}};
      });
      assert(status && found);
      bool removed = false;
      status = transaction.erase("b", &removed);
      assert(status && removed);
      return status;
    }));
  }
  // The cursor owns its Rust snapshot even after Store and Snapshot close.
  assert(retained_cursor.next([](std::string_view key, std::string_view value) {
    assert(key == "a" && value == "old");
    return kv::Status{};
  }));
  assert(retained_cursor.next([](std::string_view key, std::string_view value) {
    assert(key == "b" && value.empty());
    return kv::Status{};
  }));
  assert(retained_cursor.next([](std::string_view, std::string_view) { return kv::Status{}; }).end());
  retained_cursor = kv::Cursor{};
  kv::Store store;
  assert(kv::Store::open(argv[1], store));
  assert(store.get("a", [](std::string_view value) {
    assert(value == "new");
    return kv::Status{};
  }));
  assert(store.get("b", [](std::string_view) { return kv::Status{}; }).not_found());
  return 0;
}
