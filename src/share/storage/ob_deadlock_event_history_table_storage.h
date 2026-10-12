/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef OCEANBASE_SHARE_STORAGE_OB_DEADLOCK_EVENT_HISTORY_TABLE_STORAGE_H_
#define OCEANBASE_SHARE_STORAGE_OB_DEADLOCK_EVENT_HISTORY_TABLE_STORAGE_H_

#include "lib/ob_define.h"
#include "lib/container/ob_iarray.h"
#include "share/storage/local_store.h"
#include <functional>

namespace oceanbase
{
namespace share
{
namespace deadlock_history
{
enum Column : uint32_t { EventId, DetectorId, ReportTime, CycleIdx, CycleSize, Role, PriorityLevel, Priority, CreateTime, StartDelay, Module, Visitor, Object, ExtraName1, ExtraValue1, ExtraName2, ExtraValue2, ExtraName3, ExtraValue3 };
inline const local::Schema &schema()
{
  static const auto schema = local_schema(2, {
    {local::Kind::Integer, false}, // event_id
    {local::Kind::Integer, false}, // detector_id
    {local::Kind::Integer, false}, // report_time
    {local::Kind::Integer, false}, // cycle_idx
    {local::Kind::Integer, false}, // cycle_size
    {local::Kind::Text, true}, // role
    {local::Kind::Text, true}, // priority_level
    {local::Kind::Integer, false}, // priority
    {local::Kind::Integer, false}, // create_time
    {local::Kind::Integer, false}, // start_delay
    {local::Kind::Text, true}, // module
    {local::Kind::Text, true}, // visitor
    {local::Kind::Text, true}, // object
    {local::Kind::Text, true}, // extra_name1
    {local::Kind::Text, true}, // extra_value1
    {local::Kind::Text, true}, // extra_name2
    {local::Kind::Text, true}, // extra_value2
    {local::Kind::Text, true}, // extra_name3
    {local::Kind::Text, true}, // extra_value3
  }, {EventId, DetectorId}, {ReportTime});
  return schema;
}
} // namespace deadlock_history

struct ObDeadlockEventHistoryEntry
{
  
  uint64_t event_id_;
  uint64_t detector_id_;
  int64_t report_time_;
  int64_t cycle_idx_;
  int64_t cycle_size_;
  common::ObString role_;
  common::ObString priority_level_;
  uint64_t priority_;
  int64_t create_time_;
  uint64_t start_delay_;
  common::ObString module_;
  common::ObString visitor_;
  common::ObString object_;
  common::ObString extra_name1_;
  common::ObString extra_value1_;
  common::ObString extra_name2_;
  common::ObString extra_value2_;
  common::ObString extra_name3_;
  common::ObString extra_value3_;

  ObDeadlockEventHistoryEntry()
    : event_id_(0),
      detector_id_(0),
      report_time_(0),
      cycle_idx_(0),
      cycle_size_(0),
      role_(),
      priority_level_(),
      priority_(0),
      create_time_(0),
      start_delay_(0),
      module_(),
      visitor_(),
      object_(),
      extra_name1_(),
      extra_value1_(),
      extra_name2_(),
      extra_value2_(),
      extra_name3_(),
      extra_value3_()
  {}

  void reset() {
    
    event_id_ = 0;
    detector_id_ = 0;
    report_time_ = 0;
    cycle_idx_ = 0;
    cycle_size_ = 0;
    role_.reset();
    priority_level_.reset();
    priority_ = 0;
    create_time_ = 0;
    start_delay_ = 0;
    module_.reset();
    visitor_.reset();
    object_.reset();
    extra_name1_.reset();
    extra_value1_.reset();
    extra_name2_.reset();
    extra_value2_.reset();
    extra_name3_.reset();
    extra_value3_.reset();
  }
  TO_STRING_EMPTY();
};

class ObDeadlockEventHistoryTableStorage
{
public:
  ObDeadlockEventHistoryTableStorage();
  virtual ~ObDeadlockEventHistoryTableStorage();

  int init(LocalStore *store);
  bool is_inited() const { return nullptr != store_; }

  int insert(const ObDeadlockEventHistoryEntry &entry);
  int insert_all(const ObIArray<ObDeadlockEventHistoryEntry> &entries);
  int delete_expired(int64_t report_time_before, int64_t limit);

private:
  LocalStore *store_;
  DISALLOW_COPY_AND_ASSIGN(ObDeadlockEventHistoryTableStorage);
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_STORAGE_OB_DEADLOCK_EVENT_HISTORY_TABLE_STORAGE_H_
