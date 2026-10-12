// Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0
#include "local_bridge.h"
#include <cassert>
#include <utility>
using namespace oceanbase::local;

static Value integer(int64_t n) { return {Kind::Integer, n, {}}; }
static Schema schema(uint8_t id)
{
  Schema s;
  s.namespace_id = id;
  s.columns.push_back({"id", Kind::Integer, false});
  s.columns.push_back({"time", Kind::Integer, false});
  s.columns.push_back({"payload", Kind::Blob, true});
  s.primary.push_back(0);
  s.indexes.push_back(1);
  return s;
}
static Query all(uint8_t id)
{
  Query q;
  q.schema = schema(id);
  q.columns.push_back(0);
  q.columns.push_back(1);
  q.columns.push_back(2);
  q.order_column = -1;
  q.limit = -1;
  q.aggregate = Aggregate::None;
  return q;
}
static Mutation insert(uint8_t ns, int64_t id, int64_t time)
{
  Mutation m;
  m.action = Action::Insert;
  m.query = all(ns);
  m.fields.push_back({0, integer(id), -1});
  m.fields.push_back({1, integer(time), -1});
  Value blob{Kind::Blob, 0, {}};
  for (uint8_t byte : {0, 255, 17, 0}) { blob.bytes.push_back(byte); }
  m.fields.push_back({2, std::move(blob), -1});
  return m;
}
static void equals(Query &q, uint32_t column, int64_t value)
{
  Condition condition;
  condition.column = column;
  condition.compare = Compare::Equal;
  condition.values.push_back(integer(value));
  q.conditions.push_back(std::move(condition));
}
static int32_t count(uint8_t ns)
{
  auto q = all(ns);
  auto cursor = cursor_open(q);
  assert(cursor->status() == 0);
  int32_t n = 0;
  while (cursor->next() == 0) { ++n; }
  assert(cursor->status() == static_cast<int32_t>(Code::End));
  return n;
}
int main(int argc, char **argv)
{
  assert(argc == 2);
  assert(open_storage(argv[1]) == 0);
  auto row = insert(37, -9, 10);
  assert(write(WriteCallback([&](const Transaction &tx) {
    assert(tx.execute(row).code == 0);
    auto q = all(37);
    int visited = 0;
    assert(tx.query(q, RowCallback([&](rust::Slice<const Value> values) {
      assert(values.size() == 3 && values[0].integer == -9);
      assert(values[2].bytes.size() == 4 && values[2].bytes[1] == 255);
      ++visited;
      return static_cast<int32_t>(Code::End);
    })) == 0);
    assert(visited == 1);
    assert(write(WriteCallback([](const Transaction &) { return 0; })) == static_cast<int32_t>(Code::Busy));
    assert(close_storage() == static_cast<int32_t>(Code::Busy));
    return 0;
  })) == 0);
  // The cursor owns a descriptor copy and a stable snapshot after q is destroyed.
  {
  auto snapshot = [] { auto q = all(37); return cursor_open(q); }();
  row.action = Action::Upsert;
  row.fields[1].value.integer = 20;
  assert(write(WriteCallback([&](const Transaction &tx) { return tx.execute(row).code; })) == 0);
  assert(snapshot->next() == 0 && snapshot->row()[1].integer == 10);
  auto index_query = all(37);
  index_query.order_column = 1;
  equals(index_query, 1, 10);
  assert(cursor_open(index_query)->next() == static_cast<int32_t>(Code::End));

  // Even if C++ ignores an error, all preceding writes across namespaces roll back.
  auto other = insert(38, 1, 1);
  row.action = Action::Insert;
  assert(write(WriteCallback([&](const Transaction &tx) {
    assert(tx.execute(other).code == 0);
    assert(tx.execute(row).code == static_cast<int32_t>(Code::Duplicate));
    return 0;
  })) == static_cast<int32_t>(Code::Duplicate));
  assert(count(37) == 1 && count(38) == 0);

  // A row callback cannot mutate the transaction whose view it is visiting.
  assert(write(WriteCallback([&](const Transaction &tx) {
    assert(tx.execute(other).code == 0);
    auto q = all(37);
    assert(tx.query(q, RowCallback([&](rust::Slice<const Value>) {
      assert(tx.execute(other).code == static_cast<int32_t>(Code::Busy));
      return 0;
    })) == static_cast<int32_t>(Code::Busy));
    return 0;
  })) == static_cast<int32_t>(Code::Busy));
  assert(count(38) == 0);

  auto invalid = all(37);
  invalid.schema.primary.clear();
  assert(cursor_open(invalid)->status() == static_cast<int32_t>(Code::Invalid));
  invalid = all(37);
  invalid.schema.indexes.push_back(2); // Binary columns cannot be ordered indexes.
  assert(cursor_open(invalid)->status() == static_cast<int32_t>(Code::Invalid));
  invalid = all(37);
  invalid.schema.columns[0].nullable = true;
  assert(cursor_open(invalid)->status() == static_cast<int32_t>(Code::Invalid));
  invalid = all(37);
  invalid.aggregate = static_cast<Aggregate>(255);
  assert(cursor_open(invalid)->status() == static_cast<int32_t>(Code::Invalid));
  invalid = all(37);
  equals(invalid, 99, 1);
  assert(cursor_open(invalid)->status() == static_cast<int32_t>(Code::Invalid));
  }
  assert(close_storage() == 0);
  assert(open_storage(argv[1]) == 0);
  assert(count(37) == 1 && count(38) == 0);
  assert(close_storage() == 0);
}
