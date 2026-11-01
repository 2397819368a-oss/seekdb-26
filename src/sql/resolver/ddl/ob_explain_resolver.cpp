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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;

// DuckDB 兼容：DESCRIBE SELECT 返回结果集的 schema（column_name/column_type/null/key/default/extra）。
static int describe_type_name(const ObObjType type, ObString &name)
{
  switch (type) {
    case ObTinyIntType:
    case ObUTinyIntType:
      name = ObString::make_string("BOOLEAN"); break;
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
    case ObIntType:
    case ObUSmallIntType:
    case ObUMediumIntType:
    case ObUInt32Type:
    case ObUInt64Type:
      name = ObString::make_string("BIGINT"); break;
    case ObFloatType:
    case ObDoubleType:
    case ObUFloatType:
    case ObUDoubleType:
    case ObNumberType:
    case ObUNumberType:
    case ObDecimalIntType:
      name = ObString::make_string("DOUBLE"); break;
    case ObDateType:
      name = ObString::make_string("DATE"); break;
    case ObDateTimeType:
    case ObTimestampType:
      name = ObString::make_string("DATETIME"); break;
    default:
      name = ObString::make_string("VARCHAR"); break;
  }
  return OB_SUCCESS;
}

int ObExplainResolver::resolve_describe_select(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
  ObStmt *child_stmt = NULL;
  if (OB_ISNULL(child_node)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (T_SELECT != child_node->type_) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "DESCRIBE on non-SELECT statement");
  } else {
    ObResolver resolver(params_);
    if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, *child_node, child_stmt))) {
    } else if (OB_ISNULL(child_stmt) || stmt::T_SELECT != child_stmt->get_stmt_type()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObSelectStmt *select = static_cast<ObSelectStmt *>(child_stmt);
      ObSqlString sql;
      const int64_t count = select->get_select_item_size();
      for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        const SelectItem &item = select->get_select_item(i);
        ObString col_name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
        ObObjType col_type = ObVarcharType;
        bool nullable = true;
        if (OB_NOT_NULL(item.expr_)) {
          col_type = item.expr_->get_result_type().get_type();
          nullable = (0 == (item.expr_->get_result_flag() & NOT_NULL_FLAG));
        }
        ObString type_name;
        describe_type_name(col_type, type_name);
        if (i > 0) { OZ (sql.append(" UNION ALL ")); }
        OZ (sql.append("SELECT "));
        OZ (sql.append("'"));
        // 简单转义单引号
        for (int64_t c = 0; OB_SUCC(ret) && c < col_name.length(); ++c) {
          if (col_name.ptr()[c] == '\'') { OZ (sql.append("''")); }
          else { OZ (sql.append(col_name.ptr() + c, 1)); }
        }
        OZ (sql.append("' AS column_name,'"));
        OZ (sql.append(type_name.ptr(), type_name.length()));
        OZ (sql.append("' AS column_type,'"));
        OZ (sql.append(nullable ? "YES" : "NO"));
        OZ (sql.append("' AS `null`,NULL AS `key`,NULL AS `default`,NULL AS extra"));
      }
      if (OB_SUCC(ret) && 0 == count) {
        OZ (sql.append("SELECT NULL AS column_name,NULL AS column_type,NULL AS `null`,"
                       "NULL AS `key`,NULL AS `default`,NULL AS extra WHERE 1=0"));
      }
      if (OB_SUCC(ret)) {
        ParseResult select_result;
        ObParser parser(*params_.allocator_, session_info_->get_sql_mode());
        if (OB_FAIL(parser.parse(sql.string(), select_result))) {
        } else if (OB_ISNULL(select_result.result_tree_)
                   || select_result.result_tree_->num_child_ != 1
                   || OB_ISNULL(select_result.result_tree_->children_)
                   || OB_ISNULL(select_result.result_tree_->children_[0])) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ParseNode *select_stmt_node = select_result.result_tree_->children_[0];
          ObSelectResolver select_resolver(params_);
          if (OB_FAIL(select_resolver.resolve(*select_stmt_node))) {
          } else {
            set_basic_stmt(select_resolver.get_stmt());
          }
        }
      }
    }
  }
  return ret;
}

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL != parse_tree.children_ && NULL != parse_tree.children_[0]
             && T_EXPLAIN_DESCRIBE == parse_tree.children_[0]->type_) {
    // DESCRIBE SELECT ... ：返回结果集 schema，而不是执行计划。
    ret = resolve_describe_select(parse_tree);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
