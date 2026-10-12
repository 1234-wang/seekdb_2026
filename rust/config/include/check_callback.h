// Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <functional>
#include <utility>

namespace oceanbase { namespace config {
class CheckCallback {
public:
  explicit CheckCallback(std::function<int32_t()> callback)
    : callback_(std::move(callback)) {}
  int32_t check() const { return callback_(); }
private:
  std::function<int32_t()> callback_;
};
} } // namespace oceanbase::config
