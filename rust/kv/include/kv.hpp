// Copyright (c) 2026 OceanBase.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "kv.h"
#include <memory>
#include <string_view>
#include <utility>

namespace kv {

struct Status {
  KvStatus value{KvCode_Ok, 0};
  explicit operator bool() const noexcept { return value.code == KvCode_Ok; }
  bool not_found() const noexcept { return value.code == KvCode_NotFound; }
  bool end() const noexcept { return value.code == KvCode_End; }
};

using CommitInfo = KvCommit;
using Limits = KvLimits;
using Range = KvRange;

namespace detail {
inline KvBytes bytes(std::string_view value) noexcept
{
  return {reinterpret_cast<const uint8_t *>(value.data()), value.size()};
}
inline std::string_view view(KvBytes value) noexcept
{
  if (value.length == 0) { return {}; }
  return {reinterpret_cast<const char *>(value.data), value.length};
}
template <class F> KvStatus value(KvBytes bytes, void *context) noexcept
{
  return (*static_cast<F *>(context))(view(bytes)).value;
}
template <class F> KvStatus row(KvBytes key, KvBytes bytes, void *context) noexcept
{
  return (*static_cast<F *>(context))(view(key), view(bytes)).value;
}
} // namespace detail

inline Range all() noexcept
{
  return {KvBoundKind_Unbounded, {nullptr, 0}, KvBoundKind_Unbounded, {nullptr, 0}};
}

class Cursor {
public:
  // Views passed to the visitor remain valid until it returns. End is a
  // distinct status; a row is consumed even when the visitor returns an error.
  template <class F> Status next(F visitor)
  {
    return {kv_cursor_next(handle_.get(), &detail::row<F>, &visitor)};
  }

private:
  friend class Snapshot;
  std::unique_ptr<KvCursor, decltype(&kv_cursor_close)> handle_{nullptr, &kv_cursor_close};
};

class Snapshot {
public:
  template <class F> Status get(std::string_view key, F visitor) const
  {
    return {kv_snapshot_get(handle_.get(), detail::bytes(key), &detail::value<F>, &visitor)};
  }
  Status scan(Range range, Cursor &output) const
  {
    KvCursor *cursor = nullptr;
    Status status{kv_scan(handle_.get(), range, &cursor)};
    if (status) { output.handle_.reset(cursor); }
    return status;
  }
  Status prefix(std::string_view prefix, Cursor &output) const
  {
    KvCursor *cursor = nullptr;
    Status status{kv_scan_prefix(handle_.get(), detail::bytes(prefix), &cursor)};
    if (status) { output.handle_.reset(cursor); }
    return status;
  }

private:
  friend class Store;
  std::unique_ptr<KvSnapshot, decltype(&kv_snapshot_close)> handle_{nullptr, &kv_snapshot_close};
};

class Transaction {
public:
  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;
  Status put(std::string_view key, std::string_view value)
  {
    return {kv_tx_put(handle_, detail::bytes(key), detail::bytes(value))};
  }
  Status erase(std::string_view key, bool *deleted = nullptr)
  {
    return {kv_tx_delete(handle_, detail::bytes(key), deleted)};
  }
  template <class F> Status get(std::string_view key, F visitor) const
  {
    return {kv_tx_get(handle_, detail::bytes(key), &detail::value<F>, &visitor)};
  }
  // The scan visitor must not mutate this transaction. It may return End
  // to stop successfully, then the write callback can perform further work.
  template <class F> Status scan(Range range, F visitor) const
  {
    return {kv_tx_scan(handle_, range, &detail::row<F>, &visitor)};
  }

private:
  friend class Store;
  explicit Transaction(void *handle) : handle_(handle) {}
  void *handle_;
};

class Store {
public:
  static Status open(std::string_view directory, Store &output, const Limits *limits = nullptr)
  {
    KvStore *store = nullptr;
    Status status{kv_open(detail::bytes(directory), limits, &store)};
    if (status) { output.handle_.reset(store); }
    return status;
  }
  Status snapshot(Snapshot &output) const
  {
    KvSnapshot *snapshot = nullptr;
    Status status{kv_snapshot(handle_.get(), &snapshot)};
    if (status) { output.handle_.reset(snapshot); }
    return status;
  }
  template <class F> Status get(std::string_view key, F visitor) const
  {
    return {kv_get(handle_.get(), detail::bytes(key), &detail::value<F>, &visitor)};
  }
  // Rust owns the transaction until this synchronous callback returns. Ok
  // commits; any other status rolls back. The transaction must not escape
  // the callback, and the callback must not write this store recursively.
  template <class F> Status write(F callback, CommitInfo *info = nullptr) const
  {
    auto invoke = [](void *handle, void *context) noexcept -> KvStatus {
      Transaction transaction(handle);
      return (*static_cast<F *>(context))(transaction).value;
    };
    return {kv_write(handle_.get(), invoke, &callback, info)};
  }

private:
  std::unique_ptr<KvStore, decltype(&kv_close)> handle_{nullptr, &kv_close};
};

} // namespace kv
