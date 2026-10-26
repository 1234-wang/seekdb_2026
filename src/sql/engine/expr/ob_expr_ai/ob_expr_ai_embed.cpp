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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (ob_is_string_type(types_stack[CONTENT_IDX].get_type())) {
      // support varchar/char/binary as well as TEXT/LONGTEXT content
      if (ob_is_text_tc(types_stack[CONTENT_IDX].get_type())) {
        types_stack[CONTENT_IDX].set_calc_type(ObLongTextType);
      } else {
        types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
      }
      types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else if (ob_is_json(types_stack[CONTENT_IDX].get_type())) {
      // a json array of strings means multiple inputs in one call
    } else {
      ret = OB_ERR_INVALID_TYPE_FOR_OP;
    }
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

// read one content datum: scalar string/text -> single content,
// json array of strings -> multiple contents
int ObExprAIEmbed::read_embed_contents(const ObExpr &expr, ObEvalCtx &ctx,
                                       common::MultimodeAlloctor &allocator,
                                       const int64_t content_idx,
                                       const ObDatum &content_datum,
                                       common::ObArray<common::ObString> &contents)
{
  int ret = OB_SUCCESS;
  contents.reset();
  const ObExpr *content_arg = expr.args_[content_idx];
  if (OB_ISNULL(content_arg)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ob_is_json(content_arg->datum_meta_.type_)) {
    // multi-input: content is a json array of strings
    ObIJsonBase *j_base = nullptr;
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, allocator, content_idx, j_base, is_null))) {
    } else if (OB_ISNULL(j_base) || is_null) {
      ret = OB_INVALID_ARGUMENT;
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, content json must be an array of strings");
    } else {
      ObJsonArray *content_array = static_cast<ObJsonArray *>(j_base);
      if (content_array->element_count() == 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, content array is empty");
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < content_array->element_count(); i++) {
        ObJsonNode *node = content_array->get_value(i);
        if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, content array element must be a string");
        } else if (OB_FAIL(contents.push_back(static_cast<ObJsonString *>(node)->get_str()))) {
        }
      }
    }
  } else {
    ObString content_str;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, content_datum,
                                                          content_arg->datum_meta_,
                                                          content_arg->obj_meta_.has_lob_header(),
                                                          content_str))) {
    } else if (OB_FAIL(contents.push_back(content_str))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    // null input propagates as null result
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObArray<ObString> contents;
    if (model_id.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is empty");
      res.set_null();
    } else if (OB_FAIL(read_embed_contents(expr, ctx, temp_allocator, CONTENT_IDX, *arg_content, contents))) {
    } else if (contents.count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim) && !arg_dim->is_null()) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObArray<ObString> results;
      if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
      } else if (contents.count() == 1) {
        // single input: return the vector json directly
        if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results[0]))) {
        }
      } else {
        // multi input: return an array of vectors
        ObStringBuffer result_buf(&temp_allocator);
        if (OB_FAIL(result_buf.append("["))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < results.count(); i++) {
          if (i > 0 && OB_FAIL(result_buf.append(","))) {
          } else if (OB_FAIL(result_buf.append(results[i]))) {
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(result_buf.append("]"))) {
          } else {
            ObString result_str(static_cast<int32_t>(result_buf.length()), result_buf.ptr());
            if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result_str))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t size)
{
  INIT_SUCC(ret);
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  ObBitVector *evaluated_flags = &expr.get_evaluated_flags(ctx);
  ObDatum *datums = expr.locate_batch_datums(ctx);
  ObEvalInfo &eval_info = expr.get_eval_info(ctx);
  ObEvalCtx::BatchInfoScopeGuard batch_info_guard(ctx);
  batch_info_guard.set_batch_size(size);
  ObDatumVector model_vec, content_vec, dim_vec;
  if (expr.arg_cnt_ == 3) {
    if (OB_FAIL(expr.eval_batch_param_value(ctx, skip, size, model_vec, content_vec, dim_vec))) {
    }
  } else {
    if (OB_FAIL(expr.eval_batch_param_value(ctx, skip, size, model_vec, content_vec))) {
    }
  }
  bool got_null = false;
  // reference model id / dimension used by the batched request
  ObString ref_model_id;
  int64_t ref_dim = 0;
  bool has_ref = false;
  // contents (and their original row positions) sharing one request
  ObArray<ObString> contents;
  ObArray<int64_t> row_idx;
  for (int64_t i = 0; OB_SUCC(ret) && i < size; i++) {
    if (skip.contain(i) || evaluated_flags->contain(i)) {
      continue;
    }
    const ObDatum *model_datum = model_vec.at(i);
    const ObDatum *content_datum = content_vec.at(i);
    const ObExpr *content_arg = expr.args_[CONTENT_IDX];
    if (OB_ISNULL(model_datum) || OB_ISNULL(content_datum) || OB_ISNULL(content_arg)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (model_datum->is_null() || content_datum->is_null()) {
      // null input propagates as null result
      datums[i].set_null();
      evaluated_flags->set(i);
      got_null = true;
    } else {
      ObString model_id = model_datum->get_string();
      int64_t dim = 0;
      if (expr.arg_cnt_ == 3 && OB_NOT_NULL(dim_vec.at(i)) && !dim_vec.at(i)->is_null()) {
        dim = dim_vec.at(i)->get_int();
      }
      bool can_batch = false;
      ObString content_str;
      if (ob_is_json(content_arg->datum_meta_.type_)) {
        // json array content: fall back to row evaluation
      } else if (!model_id.empty() && dim > 0) {
        if (!has_ref) {
          ref_model_id = model_id;
          ref_dim = dim;
          has_ref = true;
        }
        can_batch = (ref_model_id.case_compare(model_id) == 0 && ref_dim == dim);
      } else if (!model_id.empty()) {
        if (!has_ref) {
          ref_model_id = model_id;
          ref_dim = 0;
          has_ref = true;
        }
        can_batch = (ref_model_id.case_compare(model_id) == 0 && ref_dim == 0);
      }
      if (can_batch) {
        // batch this row into the shared request
        if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator, *content_datum,
                                                              content_arg->datum_meta_,
                                                              content_arg->obj_meta_.has_lob_header(),
                                                              content_str))) {
        } else if (content_str.empty()) {
          // empty content is an invalid input, fall back to row evaluation
          batch_info_guard.set_batch_idx(i);
          if (OB_FAIL(expr.eval_func_(expr, ctx, datums[i]))) {
          } else {
            evaluated_flags->set(i);
            if (datums[i].is_null()) {
              got_null = true;
            }
          }
        } else if (OB_FAIL(contents.push_back(content_str))) {
        } else if (OB_FAIL(row_idx.push_back(i))) {
        }
      } else {
        // different model id / dim / json content: evaluate this row individually
        batch_info_guard.set_batch_idx(i);
        if (OB_FAIL(expr.eval_func_(expr, ctx, datums[i]))) {
        } else {
          evaluated_flags->set(i);
          if (datums[i].is_null()) {
            got_null = true;
          }
        }
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (contents.count() == 0) {
    // nothing to batch, all rows were evaluated or null
  } else {
    // one http request carries all collected inputs
    ObString model_id = ref_model_id;
    ObJsonObject *config = nullptr;
    ObJsonInt *dim_json = nullptr;
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    if (OB_FAIL(ob_write_string(temp_allocator, ref_model_id, model_id))) {
    } else if (ref_dim > 0 &&
               (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config)) ||
                OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, ref_dim, dim_json)) ||
                OB_FAIL(config->add("dimensions", dim_json)))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObArray<ObString> results;
      if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
      } else if (results.count() != contents.count()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int64_t j = 0; OB_SUCC(ret) && j < row_idx.count(); j++) {
          int64_t i = row_idx.at(j);
          const ObString &result = results.at(j);
          char *buf = expr.get_str_res_mem(ctx, result.length(), i);
          if (OB_ISNULL(buf)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            MEMCPY(buf, result.ptr(), result.length());
            datums[i].set_string(ObString(result.length(), buf));
            evaluated_flags->set(i);
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && got_null) {
    eval_info.notnull_ = false;
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
