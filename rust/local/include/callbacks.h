// Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "rust/cxx.h"
#include <cstdint>
#include <functional>
#include <utility>

namespace oceanbase { namespace local {
class Transaction;
struct Value;
class WriteCallback {
public:
  explicit WriteCallback(std::function<int32_t(const Transaction &)> callback)
    : callback_(std::move(callback)) {}
  int32_t run(const Transaction &transaction) const { return callback_(transaction); }
private:
  std::function<int32_t(const Transaction &)> callback_;
};
class RowCallback {
public:
  explicit RowCallback(std::function<int32_t(rust::Slice<const Value>)> callback)
    : callback_(std::move(callback)) {}
  int32_t visit(rust::Slice<const Value> row) const { return callback_(row); }
private:
  std::function<int32_t(rust::Slice<const Value>)> callback_;
};
} } // namespace oceanbase::local
