// Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "local_bridge.h"
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <new>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace oceanbase { namespace share {

inline int local_status(int code)
{
  if (code <= 0) { return code; } // Preserve errors returned by a C++ callback.
  switch (static_cast<local::Code>(code)) {
    case local::Code::End: return OB_ITER_END;
    case local::Code::NotFound: return OB_ENTRY_NOT_EXIST;
    case local::Code::Duplicate: return OB_ERROR;
    case local::Code::Invalid: return OB_INVALID_ARGUMENT;
    case local::Code::Busy: return OB_EAGAIN;
    case local::Code::Limit: return OB_SIZE_OVERFLOW;
    case local::Code::NotInit: return OB_NOT_INIT;
    default: return OB_IO_ERROR;
  }
}

struct LocalArg {
  int parameter{-1};
  local::Kind kind{local::Kind::Null};
  int64_t integer{0};
  std::string bytes;
  LocalArg() = default;
  LocalArg(int64_t value) : kind(local::Kind::Integer), integer(value) {}
  LocalArg(const char *value) : kind(value ? local::Kind::Text : local::Kind::Null),
      bytes(value ? value : "") {}
  static LocalArg param(int index) { LocalArg value; value.parameter = index; return value; }
  local::Value wire() const
  {
    local::Value value{kind, integer, {}};
    value.bytes.reserve(bytes.size());
    for (unsigned char byte : bytes) { value.bytes.push_back(byte); }
    return value;
  }

};
inline LocalArg local_param(int index) { return LocalArg::param(index); }

class LocalBinder {
public:
  int bind_int(int32_t value) { return bind_int64(value); }
  int bind_int64(int64_t value) { values_.emplace_back(value); return OB_SUCCESS; }
  int bind_text(const char *value) { return bind_text(value, value ? strlen(value) : 0); }
  int bind_text(const char *value, int length)
  {
    return bind_bytes(value, length, local::Kind::Text);
  }
  int bind_blob(const void *value, int length)
  {
    return bind_bytes(value, length, local::Kind::Blob);
  }
  const LocalArg *get(int index) const
  {
    return index >= 0 && index < static_cast<int>(values_.size()) ? &values_[index] : nullptr;
  }
  int status() const { return status_; }
private:
  int bind_bytes(const void *value, int length, local::Kind kind)
  {
    if (length < 0 || (!value && length > 0)) { status_ = OB_INVALID_ARGUMENT; }
    if (OB_SUCCESS == status_) {
      LocalArg arg;
      arg.kind = value ? kind : local::Kind::Null;
      if (length > 0) { arg.bytes.assign(static_cast<const char *>(value), length); }
      values_.push_back(std::move(arg));
    }
    return status_;
  }
  std::vector<LocalArg> values_;
  int status_{OB_SUCCESS};
};

inline local::Schema local_schema(uint8_t namespace_id,
    std::initializer_list<local::Column> columns,
    std::initializer_list<uint32_t> primary,
    std::initializer_list<uint32_t> indexes = {})
{
  local::Schema schema;
  schema.namespace_id = namespace_id;
  for (const auto &column : columns) { schema.columns.push_back(column); }
  for (auto column : primary) { schema.primary.push_back(column); }
  for (auto column : indexes) { schema.indexes.push_back(column); }
  return schema;
}

// Business modules supply the schema; Rust owns encoding, indexes and selection.
class LocalRequest {
public:
  static LocalRequest select(const local::Schema &schema, std::initializer_list<uint32_t> columns)
  {
    LocalRequest request(schema); request.columns_ = columns; return request;
  }
  static LocalRequest insert(const local::Schema &schema, std::initializer_list<uint32_t> columns,
                             bool overwrite = false)
  {
    LocalRequest request(schema);
    request.action_ = overwrite ? local::Action::Upsert : local::Action::Insert;
    int index = 0;
    for (uint32_t column : columns) { request.set(column, local_param(index++)); }
    return request;
  }
  static LocalRequest update(const local::Schema &schema) { return LocalRequest(schema); }
  static LocalRequest erase(const local::Schema &schema)
  {
    LocalRequest request(schema); request.action_ = local::Action::Delete; return request;
  }
  LocalRequest &eq(uint32_t column, LocalArg value) { return where(column, local::Compare::Equal, std::move(value)); }
  LocalRequest &ne(uint32_t column, LocalArg value) { return where(column, local::Compare::NotEqual, std::move(value)); }
  LocalRequest &lt(uint32_t column, LocalArg value) { return where(column, local::Compare::Less, std::move(value)); }
  LocalRequest &le(uint32_t column, LocalArg value) { return where(column, local::Compare::LessEqual, std::move(value)); }
  LocalRequest &gt(uint32_t column, LocalArg value) { return where(column, local::Compare::Greater, std::move(value)); }
  LocalRequest &ge(uint32_t column, LocalArg value) { return where(column, local::Compare::GreaterEqual, std::move(value)); }
  LocalRequest &order(uint32_t column) { order_ = column; return *this; }
  LocalRequest &take(LocalArg limit) { limit_ = std::move(limit); return *this; }
  LocalRequest &skip(LocalArg offset) { offset_ = std::move(offset); return *this; }
  LocalRequest &aggregate(local::Aggregate aggregate) { aggregate_ = aggregate; return *this; }
  LocalRequest &set(uint32_t column, LocalArg value, int clamp = -1)
  {
    fields_.push_back({column, std::move(value), clamp}); return *this;
  }
  template <class A> LocalRequest &in(uint32_t column, const A &ids)
  {
    Condition condition{column, local::Compare::In, {}};
    condition.values.reserve(ids.count());
    for (int64_t i = 0; i < ids.count(); ++i) { condition.values.emplace_back(static_cast<int64_t>(ids.at(i).id())); }
    conditions_.push_back(std::move(condition)); return *this;
  }
  struct Prepared { local::Mutation mutation; };
  int prepare(const LocalBinder &binder, Prepared &output) const
  {
    if (OB_SUCCESS != binder.status()) { return binder.status(); }
    auto resolve = [&](const LocalArg &arg) { return arg.parameter < 0 ? &arg : binder.get(arg.parameter); };
    const LocalArg *limit = resolve(limit_), *offset = resolve(offset_);
    if (!limit || !offset || limit->kind != local::Kind::Integer || offset->kind != local::Kind::Integer) {
      return OB_INVALID_ARGUMENT;
    }
    auto &query = output.mutation.query;
    query.schema = schema_;
    for (auto column : columns_) { query.columns.push_back(column); }
    for (const auto &condition : conditions_) {
      local::Condition wire;
      wire.column = condition.column;
      wire.compare = condition.compare;
      for (const LocalArg &arg : condition.values) {
        const LocalArg *value = resolve(arg);
        if (!value) { return OB_INVALID_ARGUMENT; }
        wire.values.push_back(value->wire());
      }
      query.conditions.push_back(std::move(wire));
    }
    for (const Field &field : fields_) {
      const LocalArg *value = resolve(field.value);
      if (!value) { return OB_INVALID_ARGUMENT; }
      output.mutation.fields.push_back({field.column, value->wire(), field.clamp});
    }
    query.order_column = order_;
    query.limit = limit->integer;
    query.offset = static_cast<uint64_t>(std::max<int64_t>(0, offset->integer));
    query.aggregate = aggregate_;
    output.mutation.action = action_;
    return OB_SUCCESS;
  }

private:
  struct Condition { uint32_t column; local::Compare compare; std::vector<LocalArg> values; };
  struct Field { uint32_t column; LocalArg value; int clamp; };
  explicit LocalRequest(const local::Schema &schema) : schema_(schema) {}
  LocalRequest &where(uint32_t column, local::Compare compare, LocalArg value)
  {
    conditions_.push_back({column, compare, {std::move(value)}}); return *this;
  }
  local::Schema schema_;
  std::vector<uint32_t> columns_;
  std::vector<Condition> conditions_;
  std::vector<Field> fields_;
  LocalArg limit_{int64_t(-1)}, offset_{int64_t(0)};
  int32_t order_{-1};
  local::Aggregate aggregate_{local::Aggregate::None};
  local::Action action_{local::Action::Update};
};

struct LocalStmt {
  explicit LocalStmt(LocalRequest request) : request(std::move(request)) {}
  LocalRequest request;
  std::optional<rust::Box<local::Cursor>> cursor;
};

class LocalRowReader {
public:
  void set(rust::Slice<const local::Value> values) { values_ = values; index_ = 0; }
  int64_t get_int64() { return get_int64(index_++); }
  int32_t get_int() { return static_cast<int32_t>(get_int64()); }
  int64_t get_int64(int index) const
  {
    return index >= 0 && static_cast<size_t>(index) < values_.size() ? values_[index].integer : 0;
  }
  const char *get_text(int *length = nullptr) { return get_text(index_++, length); }
  const char *get_text(int index, int *length = nullptr) const
  {
    return static_cast<const char *>(get_blob(index, length));
  }
  const void *get_blob(int *length = nullptr) { return get_blob(index_++, length); }
  const void *get_blob(int index, int *length = nullptr) const
  {
    const local::Value *value = index >= 0 && static_cast<size_t>(index) < values_.size() ? &values_[index] : nullptr;
    if (length) { *length = value ? static_cast<int>(value->bytes.size()) : 0; }
    return value && value->bytes.size() > 0 ? value->bytes.data() : nullptr;
  }
  common::ObString get_string()
  {
    int length = 0; const char *bytes = get_text(&length); return common::ObString(length, bytes);
  }
private:
  rust::Slice<const local::Value> values_;
  int index_{0};
};

class LocalConnection {
public:
  using Bind = std::function<int(LocalBinder &)>;
  int transaction(const std::function<int()> &callback)
  {
    if (transaction_) { return OB_ERR_UNEXPECTED; }
    const local::WriteCallback work([&](const local::Transaction &transaction) -> int32_t {
      transaction_ = &transaction;
      const int ret = callback();
      transaction_ = nullptr;
      return ret;
    });
    return local_status(local::write(work));
  }
  int prepare_execute(const LocalRequest &request, LocalStmt *&stmt)
  {
    stmt = new (std::nothrow) LocalStmt(request);
    return stmt ? OB_SUCCESS : OB_ALLOCATE_MEMORY_FAILED;
  }
  int prepare_query(const LocalRequest &request, const Bind &bind, LocalStmt *&stmt)
  {
    // A persistent cursor cannot escape the synchronous transaction callback.
    if (transaction_) { stmt = nullptr; return OB_NOT_SUPPORTED; }
    int ret = prepare_execute(request, stmt);
    if (OB_SUCCESS == ret) {
      LocalBinder binder; LocalRequest::Prepared prepared;
      if (bind) { ret = bind(binder); }
      if (OB_SUCCESS == ret) { ret = request.prepare(binder, prepared); }
      if (OB_SUCCESS == ret) { stmt->cursor.emplace(local::cursor_open(prepared.mutation.query)); ret = local_status((*stmt->cursor)->status()); }
      if (OB_SUCCESS != ret) { delete stmt; stmt = nullptr; }
    }
    return ret;
  }
  int step_query(LocalStmt *stmt, LocalRowReader &reader)
  {
    if (!stmt) { return OB_INVALID_ARGUMENT; }
    if (!stmt->cursor) { return OB_INVALID_ARGUMENT; }
    int ret = local_status((*stmt->cursor)->next());
    reader.set((*stmt->cursor)->row()); return ret;
  }
  void finalize_query(LocalStmt *stmt) { delete stmt; }
  void finalize_execute(LocalStmt *stmt) { delete stmt; }
  int step_execute(LocalStmt *stmt, const Bind &bind = nullptr, int64_t *affected = nullptr)
  {
    return stmt ? execute(stmt->request, bind, affected) : OB_INVALID_ARGUMENT;
  }
  int execute(const LocalRequest &request, const Bind &bind = nullptr, int64_t *affected = nullptr)
  {
    if (reading_) { return OB_ERR_UNEXPECTED; }
    LocalBinder binder; LocalRequest::Prepared prepared;
    int ret = bind ? bind(binder) : OB_SUCCESS;
    if (OB_SUCCESS == ret) { ret = request.prepare(binder, prepared); }
    if (OB_SUCCESS == ret) {
      auto write = [&]() {
        const auto result = transaction_->execute(prepared.mutation);
        if (affected) { *affected = static_cast<int64_t>(result.affected); }
        return local_status(result.code);
      };
      ret = transaction_ ? write() : transaction(write);
    }
    if (OB_SUCCESS != ret && transaction_) { transaction_->abort(ret); }
    return ret;
  }
  int query(const LocalRequest &request, const Bind &bind,
            const std::function<int(LocalRowReader &)> &visitor)
  {
    if (reading_) { return OB_ERR_UNEXPECTED; }
    if (transaction_) {
      LocalBinder binder;
      LocalRequest::Prepared prepared;
      int ret = bind ? bind(binder) : OB_SUCCESS;
      if (OB_SUCCESS == ret) { ret = request.prepare(binder, prepared); }
      if (OB_SUCCESS != ret) { transaction_->abort(ret); return ret; }
      bool found = false;
      const local::RowCallback rows([&](rust::Slice<const local::Value> values) -> int32_t {
        found = true;
        LocalRowReader reader; reader.set(values);
        reading_ = true;
        const int status = visitor ? visitor(reader) : OB_SUCCESS;
        reading_ = false;
        return status == OB_ITER_END ? static_cast<int32_t>(local::Code::End) : status;
      });
      ret = local_status(transaction_->query(prepared.mutation.query, rows));
      return OB_SUCCESS == ret && !found ? OB_ENTRY_NOT_EXIST : ret;
    }
    LocalStmt *stmt = nullptr;
    int ret = prepare_query(request, bind, stmt);
    bool found = false;
    LocalRowReader reader;
    while (OB_SUCCESS == ret) {
      ret = step_query(stmt, reader);
      if (OB_SUCCESS == ret) {
        found = true;
        if (visitor) { ret = visitor(reader); }
        if (OB_ITER_END == ret) { ret = OB_SUCCESS; break; }
      } else if (OB_ITER_END == ret) { ret = found ? OB_SUCCESS : OB_ENTRY_NOT_EXIST; break; }
    }
    finalize_query(stmt);
    return ret;
  }
private:
  const local::Transaction *transaction_{nullptr};
  bool reading_{false};
};

class LocalStore {
public:
  ~LocalStore() { destroy(); }
  int init(const char *path)
  {
    if (inited_) { return OB_INIT_TWICE; }
    if (!path) { return OB_INVALID_ARGUMENT; }
    int ret = local_status(local::open_storage(path ? rust::Str(path) : rust::Str())); inited_ = OB_SUCCESS == ret; return ret;
  }
  void destroy() { if (inited_) { (void)local::close_storage(); inited_ = false; } }
  bool is_inited() const { return inited_ && local::is_open(); }
private:
  bool inited_{false};
};

// A guard only allocates a small C++ adapter. It does not open another store.
class LocalConnectionGuard {
public:
  LocalConnectionGuard() = default;
  explicit LocalConnectionGuard(LocalStore *store)
  {
    if (store && store->is_inited()) { connection_.reset(new (std::nothrow) LocalConnection); }
  }
  LocalConnectionGuard(LocalConnectionGuard &&) = default;
  LocalConnectionGuard &operator=(LocalConnectionGuard &&) = default;
  explicit operator bool() const { return bool(connection_); }
  LocalConnection *operator->() const { return connection_.get(); }
  LocalConnection *get_connection() const { return connection_.get(); }
  void release() { connection_.reset(); }
private:
  std::unique_ptr<LocalConnection> connection_;
};
} } // namespace oceanbase::share
