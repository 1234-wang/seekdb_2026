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

#define USING_LOG_PREFIX SQL_REWRITE

#include "config_bridge.h"
#include "sql/rewrite/ob_transformer_impl.h"
#include "sql/rewrite/ob_transform_view_merge.h"
#include "sql/rewrite/ob_transform_min_max.h"
#include "sql/rewrite/ob_transform_where_subquery_pullup.h"
#include "sql/rewrite/ob_transform_eliminate_outer_join.h"
#include "sql/rewrite/ob_transform_simplify_distinct.h"
#include "sql/rewrite/ob_transform_simplify_expr.h"
#include "sql/rewrite/ob_transform_simplify_groupby.h"
#include "sql/rewrite/ob_transform_simplify_subquery.h"
#include "sql/rewrite/ob_transform_simplify_winfunc.h"
#include "sql/rewrite/ob_transform_simplify_orderby.h"
#include "sql/rewrite/ob_transform_simplify_limit.h"
#include "sql/rewrite/ob_transform_query_push_down.h"
#include "sql/rewrite/ob_transform_aggr_subquery.h"
#include "sql/rewrite/ob_transform_simplify_set.h"
#include "sql/rewrite/ob_transform_project_pruning.h"
#include "sql/rewrite/ob_transform_or_expansion.h"
#include "sql/rewrite/ob_transform_join_elimination.h"
#include "sql/rewrite/ob_transform_win_magic.h"
#include "sql/rewrite/ob_transform_groupby_pullup.h"
#include "sql/rewrite/ob_transform_groupby_pushdown.h"
#include "sql/rewrite/ob_transform_subquery_coalesce.h"
#include "sql/rewrite/ob_transform_pre_process.h"
#include "sql/rewrite/ob_transform_predicate_move_around.h"
#include "sql/rewrite/ob_transform_semi_to_inner.h"
#include "sql/rewrite/ob_transform_join_limit_pushdown.h"
#include "sql/rewrite/ob_transform_temp_table.h"
#include "sql/rewrite/ob_transform_const_propagate.h"
#include "sql/rewrite/ob_transform_left_join_to_anti.h"
#include "sql/rewrite/ob_transform_count_to_exists.h"
#include "sql/rewrite/ob_transform_expr_pullup.h"
#include "sql/rewrite/ob_transform_conditional_aggr_coalesce.h"
#include "sql/rewrite/ob_transform_decorrelate.h"
#include "sql/rewrite/ob_transform_late_materialization.h"
#include "sql/rewrite/ob_transform_distinct_aggregate.h"

using namespace oceanbase::common;
using namespace oceanbase::share;

namespace oceanbase
{
namespace sql
{
int ObTransformerImpl::transform(ObDMLStmt *&stmt)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(set_transformation_parameters(stmt->get_query_ctx()))) {
  } else if (OB_FAIL(SMART_CALL(get_stmt_trans_info(stmt, true)))) {
  } else if (OB_FAIL(stmt->formalize_implicit_distinct())) {
  } else if (OB_FAIL(do_transform_pre_precessing(stmt))) {
  } else if (OB_FAIL(stmt->formalize_query_ref_exprs())) {
  } else if (OB_FAIL(stmt->formalize_stmt_expr_reference(ctx_->expr_factory_, ctx_->session_info_)) {
  } else if (OB_FAIL(do_transform(stmt))) {
  } else if (OB_FAIL(stmt->formalize_stmt_expr_reference(ctx_->expr_factory_, ctx_->session_info_)) {
  } else if (OB_FAIL(do_after_transform(stmt, ctx_->session_info_))) {
  } else {
    print_trans_stat();
  }
  return ret;
}
