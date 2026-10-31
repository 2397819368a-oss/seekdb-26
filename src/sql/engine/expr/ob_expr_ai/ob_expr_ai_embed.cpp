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
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/expr/ob_expr_util.h"

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
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (ob_is_text_tc(types_stack[CONTENT_IDX].get_type())
        && CS_TYPE_BINARY != types_stack[CONTENT_IDX].get_collation_type()) {
      // TEXT/LONGTEXT 保持原类型，执行期再读取真实字符串数据（BLOB 仍按 varchar 处理）
      types_stack[CONTENT_IDX].set_calc_type(types_stack[CONTENT_IDX].get_type());
    } else {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
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

namespace
{
// 把结果写入指定下标的输出 datum（批量场景下需要按行下标申请结果内存）
int set_embed_result(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res, int64_t batch_idx, const ObString &str)
{
  int ret = OB_SUCCESS;
  ObTextStringDatumResult text_result(expr.datum_meta_.type_, &expr, &ctx, &res);
  if (batch_idx < 0) {
    if (OB_FAIL(text_result.init(str.length()))) {
    }
  } else if (OB_FAIL(text_result.init_with_batch_idx(str.length(), batch_idx))) {
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(text_result.append(str))) {
  } else {
    text_result.set_result();
  }
  return ret;
}
} // anonymous namespace

int ObExprAIEmbed::do_embed_request(const ObExpr &expr,
                                    ObEvalCtx &ctx,
                                    const common::ObString &model_id,
                                    common::ObArray<common::ObString> &contents,
                                    const common::ObArray<int64_t> &dimensions,
                                    common::ObArray<common::ObString> &results,
                                    common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  UNUSED(ctx);
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo resolved_endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  if (OB_UNLIKELY(contents.count() != dimensions.count())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == contents.count()) {
    // 没有需要请求的行
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, allocator, resolved_endpoint))) {
  }
  if (OB_FAIL(ret) || 0 == contents.count()) {
    // 没有需要请求的行，或前面已经出错
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObAIFuncModel model(allocator, *info, resolved_endpoint);
    // 先按内容数量占位，保证结果下标与输入行一一对应
    for (int64_t i = 0; OB_SUCC(ret) && i < contents.count(); i++) {
      if (OB_FAIL(results.push_back(ObString()))) {
      }
    }
    // 同一批次内的 dimension 可能不同，按 dimension 分组请求
    ObArray<int64_t> requested_dimensions;
    for (int64_t i = 0; OB_SUCC(ret) && i < contents.count(); i++) {
      const int64_t dimension = dimensions[i];
      bool requested = false;
      for (int64_t k = 0; !requested && k < requested_dimensions.count(); k++) {
        requested = (requested_dimensions[k] == dimension);
      }
      if (requested) {
        continue;
      } else if (OB_FAIL(requested_dimensions.push_back(dimension))) {
      } else {
        ObArray<ObString> group_contents;
        ObArray<int64_t> group_rows;
        for (int64_t j = i; OB_SUCC(ret) && j < contents.count(); j++) {
          if (dimensions[j] == dimension) {
            if (OB_FAIL(group_contents.push_back(contents[j]))) {
            } else if (OB_FAIL(group_rows.push_back(j))) {
            }
          }
        }
        ObJsonObject *config = nullptr;
        if (OB_FAIL(ret)) {
        } else if (dimension > 0) {
          ObJsonInt *dim_json = nullptr;
          if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
          } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dim_json))) {
          } else if (OB_FAIL(config->add("dimensions", dim_json))) {
          }
        }
        ObArray<ObString> group_results;
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(model.call_dense_embedding_vector_v2(group_contents, config, group_results))) {
        } else if (group_results.count() != group_contents.count()) {
          // 返回条数与请求条数不一致，拒绝返回以避免结果错位
          ret = OB_ERR_UNEXPECTED;
          FORWARD_USER_ERROR(ret, "ai_embed, result count does not match input count");
        } else {
          for (int64_t k = 0; OB_SUCC(ret) && k < group_rows.count(); k++) {
            if (group_results[k].empty()) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              results[group_rows[k]] = group_results[k];
            }
          }
        }
      }
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
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObString model_id;
    ObString content;
    if (OB_FAIL(ObTextStringHelper::get_string(ctx.exec_ctx_, expr, temp_allocator,
                                               MODEL_IDX, arg_model_id, model_id))) {
    } else if (OB_FAIL(ObTextStringHelper::get_string(ctx.exec_ctx_, expr, temp_allocator,
                                                      CONTENT_IDX, arg_content, content))) {
    }
    int64_t dim = 0;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim) && !arg_dim->is_null()) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      }
    }
    if (OB_FAIL(ret)) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    } else {
      ObArray<ObString> contents;
      ObArray<int64_t> dimensions;
      ObArray<ObString> results;
      if (OB_FAIL(contents.push_back(content))) {
      } else if (OB_FAIL(dimensions.push_back(dim))) {
      } else if (OB_FAIL(do_embed_request(expr, ctx, model_id, contents, dimensions, results, temp_allocator))) {
      } else if (results.count() != 1 || results[0].empty()) {
        ret = OB_ERR_UNEXPECTED;
        res.set_null();
      } else if (OB_FAIL(set_embed_result(expr, ctx, res, -1, results[0]))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_batch_ai_embed(const ObExpr &expr,
                                       ObEvalCtx &ctx,
                                       const ObBitVector &skip,
                                       const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatum *res_datums = expr.locate_batch_datums(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  if (OB_ISNULL(res_datums)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (expr.arg_cnt_ == 3 && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObArray<ObString> contents;
    ObArray<int64_t> dimensions;
    ObArray<int64_t> rows;
    ObString model_id;
    ObDatumVector model_datums = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector content_datums = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector dim_datums;
    if (expr.arg_cnt_ == 3) {
      dim_datums = expr.args_[DIM_IDX]->locate_expr_datumvector(ctx);
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; i++) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      ObDatum *model_datum = model_datums.at(i);
      ObDatum *content_datum = content_datums.at(i);
      if (OB_ISNULL(model_datum) || OB_ISNULL(content_datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (model_datum->is_null() || content_datum->is_null()) {
        // NULL 传播：该行结果为 NULL，且不发送到模型
        res_datums[i].set_null();
        eval_flags.set(i);
        continue;
      }
      ObString cur_model;
      ObString cur_content;
      int64_t dim = 0;
      if (OB_FAIL(ObTextStringHelper::get_string(ctx.exec_ctx_, expr, temp_allocator,
                                                 MODEL_IDX, model_datum, cur_model))) {
      } else if (OB_FAIL(ObTextStringHelper::get_string(ctx.exec_ctx_, expr, temp_allocator,
                                                        CONTENT_IDX, content_datum, cur_content))) {
      } else if (cur_model.empty() || cur_content.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      } else if (expr.arg_cnt_ == 3) {
        ObDatum *dim_datum = dim_datums.at(i);
        if (OB_ISNULL(dim_datum)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (!dim_datum->is_null()) {
          dim = dim_datum->get_int();
          if (dim <= 0) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (model_id.empty()) {
        model_id = cur_model;
      } else if (model_id != cur_model) {
        // 同一批次必须使用同一个模型
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(contents.push_back(cur_content))) {
      } else if (OB_FAIL(dimensions.push_back(dim))) {
      } else if (OB_FAIL(rows.push_back(i))) {
      }
    }

    if (OB_FAIL(ret) || 0 == rows.count()) {
    } else {
      ObArray<ObString> results;
      if (OB_FAIL(do_embed_request(expr, ctx, model_id, contents, dimensions, results, temp_allocator))) {
      } else if (results.count() != rows.count()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int64_t k = 0; OB_SUCC(ret) && k < rows.count(); k++) {
          const int64_t row = rows[k];
          if (results[k].empty()) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(set_embed_result(expr, ctx, res_datums[row], row, results[k]))) {
          } else {
            eval_flags.set(row);
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    // 向量化执行时按批次聚合多行输入，减少模型请求次数
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_batch_ai_embed;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
