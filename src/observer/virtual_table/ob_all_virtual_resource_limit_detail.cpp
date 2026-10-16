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

#define USING_LOG_PREFIX SERVER

#include "observer/virtual_table/ob_all_virtual_resource_limit_detail.h"
#include "observer/virtual_table/ob_all_virtual_resource_limit.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::share;
namespace oceanbase
{
namespace observer
{

ObResourceLimitDetailTable::ObResourceLimitDetailTable()
  : ObVirtualTableScannerIterator(),
    iter_(),
    constraint_iter_(),
    logical_resources_finished_(false),
    memory_resource_index_(0)
{
}

ObResourceLimitDetailTable::~ObResourceLimitDetailTable()
{
  reset();
}

void ObResourceLimitDetailTable::reset()
{
  constraint_iter_.reset();
  iter_.reset();
  logical_resources_finished_ = false;
  memory_resource_index_ = 0;
  ObVirtualTableScannerIterator::reset();
}

int ObResourceLimitDetailTable::get_next_resource_limit_val_(int64_t &val)
{
  int ret = OB_SUCCESS;
  bool iter_next_res = false;
  while (OB_SUCC(ret)) {
    if (!iter_.is_ready()
        && OB_FAIL(iter_.set_ready(*::oceanbase::share::server_service<::oceanbase::share::ObResourceLimitCalculator>()))) {
    } else if ((iter_.get_curr_type() == 0 || iter_next_res)
               && OB_FAIL(iter_.get_next_type())) {
      if (OB_ITER_END != ret) {
      }
    } else if (!constraint_iter_.is_ready()
               && OB_FAIL(constraint_iter_.set_ready(
                   *::oceanbase::share::server_service<::oceanbase::share::ObResourceLimitCalculator>(),
                   iter_.get_curr_type()))) {
    } else if (OB_FAIL(constraint_iter_.get_next(val))) {
      if (OB_ITER_END != ret) {
      } else {
        // iter next resource
        constraint_iter_.reset();
        iter_next_res = true;
        ret = OB_SUCCESS;
      }
    } else {
      // get succeed
      break;
    }
  }
  return ret;
}

int ObResourceLimitDetailTable::inner_get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  int64_t limit_value;
  row = nullptr;
  if (OB_ISNULL(allocator_) || OB_ISNULL(cur_row_.cells_)) {
    ret = OB_NOT_INIT;
  } else {
    start_to_read_ = true;
    if (!logical_resources_finished_
        && OB_FAIL(get_next_resource_limit_val_(limit_value))) {
      if (OB_ITER_END == ret) {
        logical_resources_finished_ = true;
        ret = OB_SUCCESS;
      }
    }
    if (OB_SUCC(ret) && logical_resources_finished_) {
      MemoryQuotaSample sample;
      if (memory_resource_index_ >= ObResourceLimitTable::memory_resource_count()) {
        ret = OB_ITER_END;
      } else if (OB_FAIL(ObResourceLimitTable::get_memory_resource_sample(
                     memory_resource_index_, sample))) {
      } else {
        limit_value = sample.limit_bytes_;
      }
    }
  }
  if (OB_SUCC(ret)) {
    const int64_t col_count = output_column_ids_.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case RESOURCE_NAME: {
          cur_row_.cells_[i].set_varchar(logical_resources_finished_
              ? ObResourceLimitTable::memory_resource_name(memory_resource_index_)
              : get_logic_res_type_name(iter_.get_curr_type()));
          cur_row_.cells_[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case LIMIT_TYPE:
          cur_row_.cells_[i].set_varchar(get_constraint_type_name(
              logical_resources_finished_ ? MEMORY_CONSTRAINT : constraint_iter_.get_curr_type()));
          cur_row_.cells_[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        case LIMIT_VALUE:
          cur_row_.cells_[i].set_int(limit_value);
          break;
        default:
          ret = OB_ERR_UNEXPECTED;
          break;
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (logical_resources_finished_) {
      ++memory_resource_index_;
    }
    row = &cur_row_;
  }
  return ret;
}

} // end namespace observer
} // end namespace oceanbase
