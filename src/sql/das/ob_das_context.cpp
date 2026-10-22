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

#define USING_LOG_PREFIX SQL_DAS
#include "ob_das_context.h"
#include "share/ob_server_struct.h"
#include "sql/ob_sql_context.h"
#include "sql/das/ob_das_utils.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/optimizer/table_access_targets.h"
#include "sql/optimizer/ob_table_location.h"
#include "sql/session/ob_basic_session_info.h"
namespace oceanbase
{
using namespace common;
using namespace share;
namespace sql
{

int ObDASCtx::validate_tablet_access(uint64_t ref_table_id,
                                    const ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_virtual_table(ref_table_id))
      && OB_UNLIKELY(tablet_id.id() != 1
                     && tablet_id.id() != EMPTY_VIRTUAL_TABLE_TABLET_ID)) {
    ret = OB_LOCATION_NOT_EXIST;
  }
  record_access_error(ret);
  return ret;
}

bool ObDASCtx::is_tablet_retry_error(int err_no) const
{
  return is_master_changed_error(err_no)
      || is_partition_change_error(err_no)
      || is_get_location_timeout_error(err_no);
}

void ObDASCtx::set_retry_info(const ObQueryRetryInfo *retry_info)
{
  if (OB_NOT_NULL(retry_info)) {
    last_retry_error_ = retry_info->get_last_query_retry_err();
    history_retry_cnt_ = retry_info->get_retry_cnt();
  }
}

void ObDASCtx::record_access_error(int err_no)
{
  if (is_tablet_retry_error(err_no)) {
    last_retry_error_ = err_no;
  }
}


int ObDASCtx::init(const ObPhysicalPlan &plan, ObExecContext &ctx)
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = ctx.get_physical_plan_ctx();
  ObSEArray<ObObjectID, 2> partition_ids;
  ObSEArray<ObObjectID, 2> first_level_part_ids;
  ObSEArray<ObTabletID, 2> tablet_ids;
  ObDataTypeCastParams dtc_params = ObBasicSessionInfo::create_dtc_params(ctx.get_my_session());
  const ObIArray<ObTableLocation> &normal_locations = plan.get_table_locations();
  const ObIArray<ObTableLocation> &das_locations = plan.get_das_table_locations();
  record_retry_error(ctx.get_my_session()->get_retry_info().get_last_query_retry_err());
  set_history_retry_cnt(ctx.get_my_session()->get_retry_info().get_retry_cnt());
  for (int64_t i = 0; OB_SUCC(ret) && i < das_locations.count(); ++i) {
    const ObTableLocation &das_location = das_locations.at(i);
    DASRuntimeTableAccess *table_loc = nullptr;
    tablet_ids.reuse();
    partition_ids.reuse();
    first_level_part_ids.reuse();
    if (OB_FAIL(das_location.calculate_tablet_ids(ctx,
                                                  plan_ctx->get_param_store(),
                                                  tablet_ids,
                                                  partition_ids,
                                                  first_level_part_ids,
                                                  dtc_params))) {
    } else if (OB_FAIL(ensure_table_access(das_location.get_loc_meta(), table_loc))) {
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < tablet_ids.count(); ++j) {
      DASRuntimeTabletAccess *tablet_loc = nullptr;
      if (OB_FAIL(ensure_tablet_access(*table_loc, tablet_ids.at(j), tablet_loc, partition_ids.at(j),
                                      first_level_part_ids.empty() ? OB_INVALID_ID : first_level_part_ids.at(j)))) {
      }
    }
  }
  return ret;
}

int ObDASCtx::get_das_tablet_mapper(const uint64_t ref_table_id,
                                    ObDASTabletMapper &tablet_mapper,
                                    const DASTableIDArrayWrap *related_table_ids)
{
  int ret = OB_SUCCESS;

  tablet_mapper.related_info_.related_map_ = &related_tablet_map_;
  tablet_mapper.related_info_.related_tids_ = related_table_ids;

  bool is_vt = is_virtual_table(ref_table_id);
  uint64_t real_table_id = ref_table_id;
  
  if (tablet_mapper.is_non_partition_optimized()) {
    // table ids has calced for no partition entity table, continue
  } else if (!is_vt) {
    //get ObTableSchema object corresponding to the table_id from ObSchemaGetterGuard
    //record the ObTableSchema into tablet_mapper
    //the tablet and partition info come from ObTableSchema in the real table
    ObSchemaGetterGuard *schema_guard = nullptr;
    if (OB_ISNULL(sql_ctx_) || OB_ISNULL(schema_guard = sql_ctx_->schema_guard_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(tablet_mapper.table_schema_)
        && OB_FAIL(schema_guard->get_table_schema( real_table_id, tablet_mapper.table_schema_))) {
    } else if (OB_ISNULL(tablet_mapper.table_schema_)) {
      ret = OB_TABLE_NOT_EXIST;
    } else {
      tablet_mapper.related_info_.guard_ = schema_guard;
    }
  } else {
    tablet_mapper.virtual_table_id_ = real_table_id;
  }
  return ret;
}

DASRuntimeTableAccess *ObDASCtx::find_table_access(uint64_t table_loc_id, uint64_t ref_table_id)
{
  DASRuntimeTableAccess *table_loc = nullptr;
  FOREACH(tmp_node, table_accesses_) {
    if ((*tmp_node)->loc_meta_->table_loc_id_ == table_loc_id &&
        (*tmp_node)->loc_meta_->ref_table_id_ == ref_table_id) {
      table_loc = *tmp_node;
      break;
    }
  }
  return table_loc;
}

int ObDASCtx::ensure_tablet_access(DASRuntimeTableAccess &table_loc,
                                  const ObTabletID &tablet_id,
                                  DASRuntimeTabletAccess *&tablet_loc,
                                  const common::ObObjectID &partition_id,
                                  const common::ObObjectID &first_level_part_id)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(table_loc.find_tablet_access(tablet_id, tablet_loc))) {
  }
  if (OB_SUCC(ret) && tablet_loc == nullptr) {
    void *loc_buf = allocator_.alloc(sizeof(DASRuntimeTabletAccess));
    if (OB_ISNULL(loc_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(tablet_loc = new(loc_buf) DASRuntimeTabletAccess())) {
      //do nothing
    } else if (OB_FAIL(validate_tablet_access(table_loc.loc_meta_->ref_table_id_, tablet_id))) {
    } else {
      tablet_loc->tablet_id_ = tablet_id;
      tablet_loc->loc_meta_ = table_loc.loc_meta_;
      tablet_loc->partition_id_ = partition_id;
      tablet_loc->first_level_part_id_ = first_level_part_id;
      if (OB_FAIL(table_loc.add_tablet_access(tablet_loc))) {
      }
    }
    //build related tablet location
    if (OB_SUCC(ret) && OB_FAIL(build_related_tablet_accesses(*tablet_loc))) {
    }
  }
  return ret;
}

int ObDASCtx::ensure_tablet_access(DASRuntimeTableAccess &table_loc,
                                  const TabletAccessTarget &tablet_target,
                                  DASRuntimeTabletAccess *&tablet_loc)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(table_loc.find_tablet_access(tablet_target.get_tablet_id(), tablet_loc))) {
  }
  if (OB_SUCC(ret) && tablet_loc == nullptr) {
    void *tablet_buf = allocator_.alloc(sizeof(DASRuntimeTabletAccess));
    if (OB_ISNULL(tablet_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      tablet_loc = new(tablet_buf) DASRuntimeTabletAccess();
      tablet_loc->tablet_id_ = tablet_target.get_tablet_id();
      tablet_loc->partition_id_ = tablet_target.get_partition_id();
      tablet_loc->first_level_part_id_ = tablet_target.get_first_level_part_id();
      tablet_loc->loc_meta_ = table_loc.loc_meta_;
      if (OB_FAIL(table_loc.add_tablet_access(tablet_loc))) {
      }
    }
    //build related tablet location
    if (OB_SUCC(ret) && OB_FAIL(build_related_tablet_accesses(*tablet_loc))) {
    }
  }
  return ret;
}

OB_INLINE int ObDASCtx::build_related_tablet_accesses(DASRuntimeTabletAccess &tablet_loc)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < tablet_loc.loc_meta_->related_table_ids_.count(); ++i) {
    ObTableID related_table_id = tablet_loc.loc_meta_->related_table_ids_.at(i);
    DASRuntimeTableAccess *related_table_loc = nullptr;
    DASRuntimeTabletAccess *related_tablet_loc = nullptr;
    const DASRelatedTabletMap::Value *rv = nullptr;
    void *related_loc_buf = allocator_.alloc(sizeof(DASRuntimeTabletAccess));
    if (OB_ISNULL(related_loc_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(related_table_loc = find_table_access(tablet_loc.loc_meta_->table_loc_id_,
                                                                 related_table_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(rv = related_tablet_map_.get_related_tablet_id(tablet_loc.tablet_id_,
                                                                        related_table_id))) {
      // Related local-index tablet pruning is available only when all operators
      // share the same DAS context.
      // A distributed plan passes tablet_id through an exchange operator,
      //but the related tablet_id map can not be passed by exchange operator,
      //unused related pruning in distributed plan's dml operator,
      //we will use get_all_tablet_and_object_id() to build the related tablet_id map when
      //dml operator's table loc was inited
      if (OB_FAIL(build_related_tablet_map(*tablet_loc.loc_meta_))) {
      } else if (OB_ISNULL(rv = related_tablet_map_.get_related_tablet_id(tablet_loc.tablet_id_,
                                                                          related_table_id))) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      related_tablet_loc = new(related_loc_buf) DASRuntimeTabletAccess();
      related_tablet_loc->tablet_id_ = rv->tablet_id_;
      related_tablet_loc->loc_meta_ = related_table_loc->loc_meta_;
      related_tablet_loc->next_ = tablet_loc.next_;
      related_tablet_loc->partition_id_ = rv->part_id_;
      related_tablet_loc->first_level_part_id_ = rv->first_level_part_id_;
      tablet_loc.next_ = related_tablet_loc;
      if (OB_FAIL(related_table_loc->add_tablet_access(related_tablet_loc))) {
      }
    }
  }
  return ret;
}

OB_INLINE int ObDASCtx::build_related_table_accesses(DASRuntimeTableAccess &table_loc)
{
  int ret = OB_SUCCESS;
  if (!table_loc.loc_meta_->related_table_ids_.empty()) {
    for (DASRuntimeTabletListIter node = table_loc.tablet_accesses_begin();
         OB_SUCC(ret) && node != table_loc.tablet_accesses_end(); ++node) {
      DASRuntimeTabletAccess *tablet_loc = *node;
      if (OB_FAIL(build_related_tablet_accesses(*tablet_loc))) {
      }
    }
  }
  return ret;
}

int ObDASCtx::ensure_table_access(const DASTableAccessMeta &loc_meta, DASRuntimeTableAccess *&table_loc)
{
  int ret = OB_SUCCESS;
  table_loc = find_table_access(loc_meta.table_loc_id_, loc_meta.ref_table_id_);
  if (nullptr == table_loc) {
    void *loc_buf = nullptr;
    if (OB_ISNULL(loc_buf = allocator_.alloc(sizeof(DASRuntimeTableAccess)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(table_loc = new(loc_buf) DASRuntimeTableAccess(allocator_))) {
      //do nothing
    } else if (OB_FAIL(table_accesses_.push_back(table_loc))) {
    } else {
      table_loc->loc_meta_ = &loc_meta;
    }
    //to extended related table location
    for (int64_t i = 0; OB_SUCC(ret) && i < loc_meta.related_table_ids_.count(); ++i) {
      ObTableID related_table_id = loc_meta.related_table_ids_.at(i);
      DASRuntimeTableAccess *related_table_loc = nullptr;
      void *related_loc_buf = allocator_.alloc(sizeof(DASRuntimeTableAccess));
      void *loc_meta_buf = allocator_.alloc(sizeof(DASTableAccessMeta));
      if (OB_ISNULL(related_loc_buf) || OB_ISNULL(loc_meta_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_ISNULL(related_table_loc = new(related_loc_buf) DASRuntimeTableAccess(allocator_))) {
        //do nothing
      } else if (OB_FAIL(table_accesses_.push_back(related_table_loc))) {
      } else {
        DASTableAccessMeta *related_loc_meta = new(loc_meta_buf) DASTableAccessMeta(allocator_);
        if (OB_FAIL(loc_meta.init_related_meta(related_table_id, *related_loc_meta))) {
        } else {
          related_table_loc->loc_meta_ = related_loc_meta;
        }
      }
    }
  }
  return ret;
}

int ObDASCtx::add_table_access_targets(const DASTableAccessMeta &loc_meta,
                                  const TableAccessTargets &table_targets)
{
  int ret = OB_SUCCESS;
  DASRuntimeTableAccess *table_loc = nullptr;
  DASTableAccessMeta *final_meta = nullptr;
  const TabletAccessTargetIArray &tablet_targets = table_targets.get_tablets();
  if (OB_FAIL(ObDASUtils::build_table_access_meta(allocator_, loc_meta, final_meta))) {
  } else if (OB_FAIL(ensure_table_access(*final_meta, table_loc))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tablet_targets.count(); ++i) {
    const TabletAccessTarget &tablet_target = tablet_targets.at(i);
    DASRuntimeTabletAccess *tablet_loc = nullptr;
    if (OB_FAIL(ensure_tablet_access(*table_loc, tablet_target, tablet_loc))) {
    }
  }
  LOG_DEBUG("das table loc assign finish", K(table_targets), K(loc_meta), K(table_loc->get_tablet_accesses()));
  return ret;
}

int ObDASCtx::add_final_table_access(const DASTableAccessMeta &loc_meta,
                                  const ObIArray<ObTabletID> &tablet_ids,
                                  const ObIArray<ObObjectID> &partition_ids,
                                  const ObIArray<ObObjectID> &first_level_part_ids)
{
  int ret = OB_SUCCESS;
  DASRuntimeTableAccess *table_loc = nullptr;
  DASTableAccessMeta *final_meta = nullptr;
  if (OB_FAIL(ObDASUtils::build_table_access_meta(allocator_, loc_meta, final_meta))) {
  } else if (OB_FAIL(ensure_table_access(*final_meta, table_loc))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tablet_ids.count(); ++i) {
    DASRuntimeTabletAccess *tablet_loc = nullptr;
    ObObjectID first_level_part_id =
        first_level_part_ids.empty() ? OB_INVALID_ID : first_level_part_ids.at(i);
    if (OB_FAIL(ensure_tablet_access(*table_loc,
                                    tablet_ids.at(i),
                                    tablet_loc,
                                    partition_ids.at(i),
                                    first_level_part_id))) {
    }
  }
  LOG_DEBUG("das table loc assign finish", K(loc_meta), K(table_loc->get_tablet_accesses()));

  if (OB_FAIL(ret)) {
    clear_table_accesses();
  }
  return ret;
}

int ObDASCtx::build_table_access_meta(const DASTableAccessMeta &src,
                                    DASTableAccessMeta *&dst)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObDASUtils::build_table_access_meta(allocator_, src, dst))) {
  }
  return ret;
}

int64_t ObDASCtx::get_related_tablet_cnt() const
{
  int64_t total_cnt = 0;
  FOREACH(table_node, table_accesses_) {
    DASRuntimeTableAccess *table_loc = *table_node;
    total_cnt += table_loc->get_tablet_accesses().size();
  }

  return total_cnt;
}

int ObDASCtx::rebuild_tablet_access_links()
{
  int ret = OB_SUCCESS;
  FOREACH_X(table_node, table_accesses_, OB_SUCC(ret)) {
    DASRuntimeTableAccess *table_loc = *table_node;
    ObTableID table_loc_id = table_loc->loc_meta_->table_loc_id_;
    if (table_loc->rebuild_reference_) {
      //has been rebuild the related table reference, ignore it
      continue;
    } else {
      table_loc->rebuild_reference_ = 1;
    }
    for (int64_t i = 0; i < table_loc->loc_meta_->related_table_ids_.count(); ++i) {
      ObTableID related_table_id = table_loc->loc_meta_->related_table_ids_.at(i);
      DASRuntimeTableAccess *related_table_loc = find_table_access(table_loc_id, related_table_id);
      related_table_loc->rebuild_reference_ = 1;
      if (table_loc->get_tablet_accesses().size() != related_table_loc->get_tablet_accesses().size()) {
        ret = OB_ERR_UNEXPECTED;
      }
      DASRuntimeTabletList::iterator tablet_iter = table_loc->tablet_accesses_begin();
      DASRuntimeTabletList::iterator related_tablet_iter = related_table_loc->tablet_accesses_begin();
      for (; OB_SUCC(ret) && tablet_iter != table_loc->tablet_accesses_end();
          ++tablet_iter, ++related_tablet_iter) {
        DASRuntimeTabletAccess *tablet_loc = *tablet_iter;
        DASRuntimeTabletAccess *related_tablet_loc = *related_tablet_iter;
        related_tablet_loc->next_ = tablet_loc->next_;
        tablet_loc->next_ = related_tablet_loc;
      }
    }
  }
  return ret;
}

int ObDASCtx::build_related_tablet_map(const DASTableAccessMeta &loc_meta)
{
  int ret = OB_SUCCESS;
  ObDASTabletMapper tablet_mapper;
  ObArray<ObTabletID> tablet_ids;
  ObArray<ObObjectID> partition_ids;
  if (OB_FAIL(get_das_tablet_mapper(loc_meta.ref_table_id_, tablet_mapper, &loc_meta.related_table_ids_))) {
  } else if (OB_FAIL(tablet_mapper.get_all_tablet_and_object_id(tablet_ids, partition_ids))) {
  }
  return ret;
}

int ObDASCtx::find_group_param_by_param_idx(int64_t param_idx,
                                    bool &exist, uint64_t &array_idx)
{
  int ret = OB_SUCCESS;
  exist = false;
  array_idx = OB_INVALID_ID;
  if(OB_ISNULL(group_params_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < group_params_->count() && !exist; ++i) {
      const GroupRescanParam &group_param = group_params_->at(i);
      if (param_idx == group_param.param_idx_) {
        exist = true;
        array_idx = i;
      }
    }
  }
  return ret;
}

OB_DEF_SERIALIZE(ObDASCtx)
{
  int ret = OB_SUCCESS;
  OB_UNIS_ENCODE(table_accesses_.size());
  FOREACH_X(tmp_node, table_accesses_, OB_SUCC(ret)) {
    DASRuntimeTableAccess *table_loc = *tmp_node;
    OB_UNIS_ENCODE(*table_loc);
  }
  OB_UNIS_ENCODE(flags_);
  OB_UNIS_ENCODE(snapshot_);
  OB_UNIS_ENCODE(write_branch_id_);
  return ret;
}

OB_DEF_DESERIALIZE(ObDASCtx)
{
  int ret = OB_SUCCESS;
  int64_t size = 0;
  OB_UNIS_DECODE(size);
  for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
    DASRuntimeTableAccess *table_loc = nullptr;
    void *table_buf = allocator_.alloc(sizeof(DASRuntimeTableAccess));
    if (OB_ISNULL(table_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      table_loc = new(table_buf) DASRuntimeTableAccess(allocator_);
      if (OB_FAIL(table_accesses_.push_back(table_loc))) {
      }
    }
    OB_UNIS_DECODE(*table_loc);
    OX(table_loc->rebuild_reference_ = 0);
  }
  OB_UNIS_DECODE(flags_);
  OB_UNIS_DECODE(snapshot_);
  if (OB_SUCC(ret) && OB_FAIL(rebuild_tablet_access_links())) {
  }
  OB_UNIS_DECODE(write_branch_id_);
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObDASCtx)
{
  int64_t len = 0;
  OB_UNIS_ADD_LEN(table_accesses_.size());
  FOREACH(tmp_node, table_accesses_) {
    DASRuntimeTableAccess *table_loc = *tmp_node;
    OB_UNIS_ADD_LEN(*table_loc);
  }
  OB_UNIS_ADD_LEN(flags_);
  OB_UNIS_ADD_LEN(snapshot_);
  OB_UNIS_ADD_LEN(write_branch_id_);
  return len;
}
}  // namespace sql
}  // namespace oceanbase
