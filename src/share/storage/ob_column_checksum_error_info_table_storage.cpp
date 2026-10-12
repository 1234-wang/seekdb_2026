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

#define USING_LOG_PREFIX SHARE

#include "share/storage/ob_column_checksum_error_info_table_storage.h"
#include "lib/oblog/ob_log.h"
#include "lib/string/ob_sql_string.h"
#include "share/ob_column_checksum_error_operator.h"
#include "share/storage/local_store.h"

namespace oceanbase
{
namespace share
{

ObColumnChecksumErrorInfoTableStorage::ObColumnChecksumErrorInfoTableStorage()
  : pool_(nullptr)
{
}

ObColumnChecksumErrorInfoTableStorage::~ObColumnChecksumErrorInfoTableStorage()
{
}

int ObColumnChecksumErrorInfoTableStorage::init(LocalStore *pool)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(pool_ = pool)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(create_table_if_not_exists())) {
  }
  if (OB_FAIL(ret)) {
    pool_ = NULL;
  }
  return ret;
}

int ObColumnChecksumErrorInfoTableStorage::create_table_if_not_exists()
{
  return pool_ && pool_->is_inited() ? OB_SUCCESS : OB_NOT_INIT;
}

int ObColumnChecksumErrorInfoTableStorage::insert(const ObColumnChecksumErrorInfo &error_info)
{
  int ret = OB_SUCCESS;
  if (!is_inited()) {
    ret = OB_NOT_INIT;
  } else {
    const auto insert_request =
      LocalRequest::insert(checksum_error::schema(), {checksum_error::FrozenScn, checksum_error::IndexType, checksum_error::DataTableId, checksum_error::IndexTableId, checksum_error::DataTabletId, checksum_error::IndexTabletId, checksum_error::ColumnId, checksum_error::DataColumnChecksum, checksum_error::IndexColumnChecksum}, false);

    auto binder = [&](LocalBinder &b) -> int {
      b.bind_int64(error_info.frozen_scn_.get_val_for_inner_table_field());
      b.bind_int64(error_info.is_global_index_ ? 1 : 0);
      b.bind_int64(error_info.data_table_id_);
      b.bind_int64(error_info.index_table_id_);
      b.bind_int64(error_info.data_tablet_id_.id());
      b.bind_int64(error_info.index_tablet_id_.id());
      b.bind_int64(error_info.column_id_);
      b.bind_int64(error_info.data_column_checksum_);
      b.bind_int64(error_info.index_column_checksum_);
      return OB_SUCCESS;
    };

    LocalConnectionGuard guard(pool_);
    if (!guard) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(guard->execute(insert_request, binder))) {
    }
  }
  return ret;
}

int ObColumnChecksumErrorInfoTableStorage::insert_all(const ObIArray<ObColumnChecksumErrorInfo> &error_infos)
{
  int ret = OB_SUCCESS;
  if (!is_inited()) {
    ret = OB_NOT_INIT;
  } else if (error_infos.empty()) {
    // do nothing
  } else {
    const auto insert_request =
      LocalRequest::insert(checksum_error::schema(), {checksum_error::FrozenScn, checksum_error::IndexType, checksum_error::DataTableId, checksum_error::IndexTableId, checksum_error::DataTabletId, checksum_error::IndexTabletId, checksum_error::ColumnId, checksum_error::DataColumnChecksum, checksum_error::IndexColumnChecksum}, false);

    LocalConnectionGuard guard(pool_);
    if (!guard) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      // Begin transaction for batch insert
      ret = guard->transaction([&]() -> int {
        LocalStmt *stmt = nullptr;
        if (OB_FAIL(guard->prepare_execute(insert_request, stmt))) {
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < error_infos.count(); ++i) {
            const ObColumnChecksumErrorInfo &error_info = error_infos.at(i);
            auto binder = [&](LocalBinder &b) -> int {
              b.bind_int64(error_info.frozen_scn_.get_val_for_inner_table_field());
              b.bind_int64(error_info.is_global_index_ ? 1 : 0);
              b.bind_int64(error_info.data_table_id_);
              b.bind_int64(error_info.index_table_id_);
              b.bind_int64(error_info.data_tablet_id_.id());
              b.bind_int64(error_info.index_tablet_id_.id());
              b.bind_int64(error_info.column_id_);
              b.bind_int64(error_info.data_column_checksum_);
              b.bind_int64(error_info.index_column_checksum_);
              return OB_SUCCESS;
            };

            if (OB_FAIL(guard->step_execute(stmt, binder))) {
            }
          }

          // Finalize statement
          guard->finalize_execute(stmt);


        }

        return ret;
      });
    }
  }
  return ret;
}

int ObColumnChecksumErrorInfoTableStorage::get(
    const SCN &frozen_scn,
    const bool is_global_index,
    const int64_t data_table_id,
    const int64_t index_table_id,
    const common::ObTabletID &data_tablet_id,
    const common::ObTabletID &index_tablet_id,
    ObIArray<ObColumnChecksumErrorInfo> &error_infos)
{
  int ret = OB_SUCCESS;
  error_infos.reset();
  if (!is_inited()) {
    ret = OB_NOT_INIT;
  } else {
    const auto select_request =
      LocalRequest::select(checksum_error::schema(), {checksum_error::FrozenScn, checksum_error::IndexType, checksum_error::DataTableId, checksum_error::IndexTableId, checksum_error::DataTabletId, checksum_error::IndexTabletId, checksum_error::ColumnId, checksum_error::DataColumnChecksum, checksum_error::IndexColumnChecksum}).eq(checksum_error::FrozenScn, local_param(0)).eq(checksum_error::IndexType, local_param(1)).eq(checksum_error::DataTableId, local_param(2)).eq(checksum_error::IndexTableId, local_param(3)).eq(checksum_error::DataTabletId, local_param(4)).eq(checksum_error::IndexTabletId, local_param(5)).order(checksum_error::ColumnId);

    auto binder = [&](LocalBinder &b) -> int {
      b.bind_int64(frozen_scn.get_val_for_inner_table_field());
      b.bind_int64(is_global_index ? 1 : 0);
      b.bind_int64(data_table_id);
      b.bind_int64(index_table_id);
      b.bind_int64(data_tablet_id.id());
      b.bind_int64(index_tablet_id.id());
      return OB_SUCCESS;
    };

    auto row_processor = [&](LocalRowReader &reader) -> int {
      ObColumnChecksumErrorInfo error_info;
      uint64_t frozen_scn_val = reader.get_int64();
      int64_t index_type = reader.get_int64();
      int64_t data_table_id_val = reader.get_int64();
      int64_t index_table_id_val = reader.get_int64();
      int64_t data_tablet_id_val = reader.get_int64();
      int64_t index_tablet_id_val = reader.get_int64();
      int64_t column_id_val = reader.get_int64();
      int64_t data_column_checksum_val = reader.get_int64();
      int64_t index_column_checksum_val = reader.get_int64();

      error_info.frozen_scn_.convert_for_inner_table_field(frozen_scn_val);
      error_info.is_global_index_ = (index_type != 0);
      error_info.data_table_id_ = data_table_id_val;
      error_info.index_table_id_ = index_table_id_val;
      error_info.data_tablet_id_ = ObTabletID(data_tablet_id_val);
      error_info.index_tablet_id_ = ObTabletID(index_tablet_id_val);
      error_info.column_id_ = column_id_val;
      error_info.data_column_checksum_ = data_column_checksum_val;
      error_info.index_column_checksum_ = index_column_checksum_val;

      if (OB_FAIL(error_infos.push_back(error_info))) {
      }
      return ret;
    };

    LocalConnectionGuard guard(pool_);
    if (!guard) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(guard->query(select_request, binder, row_processor))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
      } else {
        ret = OB_SUCCESS; // No rows is acceptable
      }
    }
  }
  return ret;
}

int ObColumnChecksumErrorInfoTableStorage::delete_expired(
    const SCN &frozen_scn_before,
    int64_t limit)
{
  int ret = OB_SUCCESS;
  if (!is_inited()) {
    ret = OB_NOT_INIT;
  } else {
    const auto request = LocalRequest::erase(checksum_error::schema())
        .lt(checksum_error::FrozenScn, static_cast<int64_t>(frozen_scn_before.get_val_for_inner_table_field())).order(checksum_error::FrozenScn).take(limit);
    LocalConnectionGuard guard(pool_);
    if (!guard) { ret = OB_ERR_UNEXPECTED; }
    else { ret = guard->execute(request); }
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
