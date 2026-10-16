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

#ifndef OB_ALL_VIRTUAL_RESOURCE_LIMIT_H_
#define OB_ALL_VIRTUAL_RESOURCE_LIMIT_H_

#include "observer/virtual_table/ob_virtual_table_scanner_iterator.h"
#include "lib/resource/ob_memory_quota.h"
#include "share/resource_limit_calculator/ob_resource_limit_calculator.h"

namespace oceanbase
{
namespace share
{
class ObResourceInfo;
}

namespace observer
{
class ObResourceLimitTable : public common::ObVirtualTableScannerIterator
{
public:
  ObResourceLimitTable();
  virtual ~ObResourceLimitTable();
public:
  virtual int inner_get_next_row(common::ObNewRow *&row);
  virtual void reset();
  // Observation-only resources: do not register these with the calculator,
  // which also converts tablet counts into physical resource requirements.
  static int64_t memory_resource_count() { return 4; }
  static const char *memory_resource_name(int64_t index);
  static int get_memory_resource_sample(int64_t index,
      common::MemoryQuotaSample &sample);
  enum COLUMN_NAME
  {
        RESOURCE_NAME = common::OB_APP_MIN_COLUMN_ID,
    CURRENT_UTILIZATION,
    MAX_UTILIZATION,
    RSERVED_VALUE,
    LIMIT_VALUE,
    EFFECTIVE_LIMIT_TYPE
  };
private:
  int get_next_resource_info_(share::ObResourceInfo &info);
private:
  share::ObLogicResourceStatIterator iter_;
  bool logical_resources_finished_;
  int64_t memory_resource_index_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObResourceLimitTable);
};

} // end namespace observer
} // end namespace oceanbase

#endif
