/**
 * AI column background worker.
 */
#define USING_LOG_PREFIX SHARE

#include "observer/ai_service/ob_ai_column_worker.h"

#include "lib/allocator/page_arena.h"
#include "lib/ob_errno.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/object/ob_obj_type.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace observer
{
using common::ObArenaAllocator;
using common::ObIArray;
using common::ObMySQLProxy;
using common::ObSEArray;
using common::ObSqlString;
using common::ObString;
using common::sqlclient::ObMySQLResult;
using share::schema::ObAiColumnInfo;
using share::schema::ObColumnSchemaV2;
using share::schema::ObDatabaseSchema;
using share::schema::ObSchemaGetterGuard;
using share::schema::ObTableSchema;

namespace
{
const int64_t AI_COLUMN_WORKER_INTERVAL = 100L * 1000L; // 100ms
const char *const TASK_TABLE = "oceanbase.__all_ai_column_task";

bool ai_worker_same_string(const ObString &lhs, const ObString &rhs)
{
  return lhs.length() == rhs.length()
      && (0 == lhs.length() || 0 == MEMCMP(lhs.ptr(), rhs.ptr(), lhs.length()));
}

int ai_worker_split_expr(const ObString &normalized_expr, ObString &expr, ObString &hint)
{
  int ret = OB_SUCCESS;
  int64_t nl_pos = -1;
  for (int64_t i = normalized_expr.length() - 1; i >= 0; --i) {
    if ('\n' == normalized_expr.ptr()[i]) {
      nl_pos = i;
      break;
    }
  }
  if (nl_pos < 0) {
    expr = normalized_expr;
    hint.reset();
  } else {
    expr.assign_ptr(normalized_expr.ptr(), static_cast<int32_t>(nl_pos));
    hint.assign_ptr(normalized_expr.ptr() + nl_pos + 1,
                    static_cast<int32_t>(normalized_expr.length() - nl_pos - 1));
  }
  return ret;
}

// Build the value expression assigned to the ai column:
//   json column     -> cast(<expr> as json)
//   integer column  -> cast(json_unquote(<expr>) as signed)
//   other columns   -> <expr>
// The type hint stored together with the expression is passed to the model as
// the system message through the ai_complete config argument.
int ai_worker_build_value(ObSqlString &out,
                          const ObString &raw_expr,
                          const ObString &hint,
                          const ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  ObString trimmed_hint = hint;
  while (trimmed_hint.length() > 0
         && (' ' == trimmed_hint.ptr()[0] || '\r' == trimmed_hint.ptr()[0]
             || '\n' == trimmed_hint.ptr()[0])) {
    trimmed_hint.assign_ptr(trimmed_hint.ptr() + 1, trimmed_hint.length() - 1);
  }
  ObSqlString expr_with_system;
  ObString expr = raw_expr;
  if (!trimmed_hint.empty()) {
    int64_t last_paren = -1;
    for (int64_t i = raw_expr.length() - 1; i >= 0; --i) {
      if (')' == raw_expr.ptr()[i]) {
        last_paren = i;
        break;
      }
    }
    if (OB_UNLIKELY(last_paren < 0)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected ai column expression without closing paren",
               K(ret), K(raw_expr));
    } else if (OB_FAIL(expr_with_system.append(raw_expr.ptr(), last_paren))) {
    } else if (OB_FAIL(expr_with_system.append(", '{\"system_prompt\":\""))) {
    } else if (OB_FAIL(expr_with_system.append(trimmed_hint.ptr(),
                                               trimmed_hint.length()))) {
    } else if (OB_FAIL(expr_with_system.append("\"}')"))) {
    } else {
      expr = expr_with_system.string();
    }
  }
  if (OB_SUCC(ret)) {
    bool is_json = column.is_json();
    bool is_integer = ob_is_integer_type(column.get_data_type());
    if (is_json) {
      ret = out.append_fmt("cast(%.*s as json)", static_cast<int>(expr.length()), expr.ptr());
    } else if (is_integer) {
      ret = out.append_fmt("cast(json_unquote(%.*s) as signed)",
                           static_cast<int>(expr.length()), expr.ptr());
    } else {
      ret = out.append(expr.ptr(), expr.length());
    }
    LOG_INFO("ai_worker_build_value", K(ret), K(is_json), K(is_integer),
             KCSTRING(expr.ptr()));
  }
  return ret;
}

int ai_worker_read_int64(const ObString &sql, int64_t &value)
{
  int ret = OB_SUCCESS;
  ObMySQLProxy::MySQLResult res;
  ObMySQLResult *result = NULL;
  value = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("sql proxy is null", K(ret));
  } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr(), nullptr))) {
    LOG_WARN("failed to read ai column task sql", K(ret), KCSTRING(sql.ptr()));
  } else if (OB_ISNULL(result = res.get_result())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(result->next())) {
    LOG_WARN("failed to get ai column task row", K(ret));
  } else if (OB_FAIL(result->get_int(static_cast<int64_t>(0), value))) {
    LOG_WARN("failed to read int value", K(ret));
  }
  return ret;
}

int ai_worker_exec_write(const ObString &sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("sql proxy is null", K(ret));
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql, affected_rows))) {
    LOG_WARN("failed to write ai column task sql", K(ret), KCSTRING(sql.ptr()));
  }
  return ret;
}
} // anonymous namespace

ObAiColumnWorkerTask::ObAiColumnWorkerTask()
  : timer_(NULL),
    is_inited_(false),
    is_stopped_(false)
{
}

ObAiColumnWorkerTask::~ObAiColumnWorkerTask()
{
}

int ObAiColumnWorkerTask::init(common::ObTimer &timer)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ai column worker is already inited", K(ret));
  } else if (OB_FAIL(timer.schedule(*this, AI_COLUMN_WORKER_INTERVAL, true))) {
    LOG_WARN("failed to schedule ai column worker", K(ret));
  } else {
    timer_ = &timer;
    is_inited_ = true;
    LOG_INFO("ai column worker inited", K(AI_COLUMN_WORKER_INTERVAL));
  }
  return ret;
}

void ObAiColumnWorkerTask::stop()
{
  is_stopped_ = true;
}

void ObAiColumnWorkerTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  if (!is_inited_ || is_stopped_) {
    // nothing to do
  } else if (OB_FAIL(process_pending_tasks())) {
    LOG_WARN("failed to process ai column tasks", K(ret));
  }
}

int ObAiColumnWorkerTask::process_pending_tasks()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColWorker");
  ObSqlString sql;
  ObMySQLProxy::MySQLResult res;
  ObMySQLResult *result = NULL;
  ObSEArray<AiColTaskItem, 64> tasks;

  if (OB_ISNULL(GCTX.sql_proxy_) || OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("server is not inited", K(ret));
  } else if (OB_FAIL(sql.append_fmt(
      "select table_id, column_id, column_name, rowkey_predicate, request_id,"
      " retry_count from %s where state in (0, 3) and eligible_ts <= now(6)"
      " order by request_id, table_id limit 200", TASK_TABLE))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr(), nullptr))) {
    LOG_WARN("failed to read ai column tasks", K(ret), KCSTRING(sql.ptr()));
  } else if (OB_ISNULL(result = res.get_result())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("task query result is null", K(ret));
  } else {
    int tmp_ret = OB_SUCCESS;
    while (OB_SUCC(ret) && OB_SUCCESS == (tmp_ret = result->next())) {
      int64_t table_id = 0;
      int64_t column_id = 0;
      int64_t retry_count = 0;
      ObString column_name;
      ObString predicate;
      ObString request_id;
      AiColTaskItem item;
      if (OB_FAIL(result->get_int("table_id", table_id))) {
        LOG_WARN("failed to get table id", K(ret));
      } else if (OB_FAIL(result->get_int("column_id", column_id))) {
        LOG_WARN("failed to get column id", K(ret));
      } else if (OB_FAIL(result->get_int("retry_count", retry_count))) {
        LOG_WARN("failed to get retry count", K(ret));
      } else if (OB_FAIL(result->get_varchar("column_name", column_name))) {
        LOG_WARN("failed to get column name", K(ret));
      } else if (OB_FAIL(result->get_varchar("rowkey_predicate", predicate))) {
        LOG_WARN("failed to get rowkey predicate", K(ret));
      } else if (OB_FAIL(result->get_varchar("request_id", request_id))) {
        LOG_WARN("failed to get request id", K(ret));
      } else if (OB_FAIL(common::ob_write_string(allocator, column_name,
                                                 item.column_name_))) {
        LOG_WARN("failed to copy column name", K(ret));
      } else if (OB_FAIL(common::ob_write_string(allocator, predicate,
                                                 item.rowkey_predicate_))) {
        LOG_WARN("failed to copy rowkey predicate", K(ret));
      } else if (OB_FAIL(common::ob_write_string(allocator, request_id,
                                                 item.request_id_))) {
        LOG_WARN("failed to copy request id", K(ret));
      } else if (OB_FAIL(tasks.push_back(item))) {
        LOG_WARN("failed to push task item", K(ret));
      } else {
        AiColTaskItem &last = tasks.at(tasks.count() - 1);
        last.table_id_ = static_cast<uint64_t>(table_id);
        last.column_id_ = column_id;
        last.retry_count_ = retry_count;
      }
    }
    if (OB_SUCCESS != tmp_ret && OB_ITER_END != tmp_ret) {
      ret = tmp_ret;
    }
  }

  int64_t i = 0;
  while (OB_SUCC(ret) && i < tasks.count()) {
    int64_t j = i + 1;
    while (j < tasks.count()
           && tasks.at(i).table_id_ == tasks.at(j).table_id_
           && ai_worker_same_string(tasks.at(i).request_id_, tasks.at(j).request_id_)) {
      ++j;
    }
    int tmp_ret = process_one_group(allocator, tasks, i, j,
                                    tasks.at(i).table_id_,
                                    tasks.at(i).request_id_,
                                    tasks.at(i).rowkey_predicate_);
    if (OB_SUCCESS != tmp_ret) {
      // one broken group should not block the others
      LOG_WARN("failed to process one ai task group", K(tmp_ret),
               "table_id", tasks.at(i).table_id_,
               K(tasks.at(i).request_id_));
    }
    i = j;
  }
  return ret;
}

int ObAiColumnWorkerTask::process_one_group(ObArenaAllocator &allocator,
                                            const ObIArray<AiColTaskItem> &tasks,
                                            const int64_t start,
                                            const int64_t end,
                                            const uint64_t table_id,
                                            const ObString &request_id,
                                            const ObString &predicate)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *table_schema = NULL;
  const ObDatabaseSchema *database_schema = NULL;
  ObSqlString check_sql;
  int64_t alive_task_count = 0;
  ObSqlString set_sql;
  ObSqlString update_sql;
  ObSqlString del_sql;
  int64_t max_retries = 0;
  bool skip_group = false;

  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
    LOG_WARN("failed to get schema guard", K(ret));
  } else if (OB_FAIL(schema_guard.get_table_schema(table_id, table_schema))
             || NULL == table_schema) {
    // the table has been dropped or altered, the pending tasks are useless
    LOG_INFO("ai task table not found, drop tasks", K(ret), K(table_id));
    ret = OB_SUCCESS;
    skip_group = true;
    del_sql.assign_fmt("delete from %s where table_id = %lu and request_id = '%.*s'",
                       TASK_TABLE, static_cast<unsigned long>(table_id),
                       static_cast<int>(request_id.length()), request_id.ptr());
    int tmp_ret = ai_worker_exec_write(del_sql.string());
    LOG_INFO("ai_worker_drop_tasks", K(tmp_ret), K(table_id), K(request_id));
  } else if (OB_FAIL(schema_guard.get_database_schema(table_schema->get_database_id(),
                                                      database_schema))
             || NULL == database_schema) {
    LOG_WARN("failed to get database schema", K(ret), K(table_id));
  } else if (OB_FAIL(check_sql.assign_fmt(
      "select count(*) from %s where table_id = %lu and request_id = '%.*s'",
      TASK_TABLE, static_cast<unsigned long>(table_id),
      static_cast<int>(request_id.length()), request_id.ptr()))) {
  } else if (OB_FAIL(ai_worker_read_int64(check_sql.string(), alive_task_count))) {
  } else if (alive_task_count != end - start) {
    // some tasks of this group have been replaced by a newer update, the
    // values computed for the older row version must be discarded.
    LOG_INFO("ai task group is stale, skip", K(table_id), K(request_id),
             K(alive_task_count), "task_count", end - start);
    skip_group = true;
  } else {
    LOG_INFO("ai_worker_process_group", K(table_id), K(request_id),
             K(predicate), "task_count", end - start);
  }

  for (int64_t k = start; OB_SUCC(ret) && !skip_group && k < end; ++k) {
    const AiColTaskItem &item = tasks.at(k);
    const ObAiColumnInfo *ai_info = NULL;
    const ObColumnSchemaV2 *ai_column = NULL;
    const ObIArray<ObAiColumnInfo> &ai_infos = table_schema->get_ai_column_infos();
    for (int64_t a = 0; a < ai_infos.count() && NULL == ai_info; ++a) {
      if (ai_infos.at(a).column_id_ == static_cast<uint64_t>(item.column_id_)) {
        ai_info = &ai_infos.at(a);
      }
    }
    if (NULL == ai_info
        || NULL == (ai_column = table_schema->get_column_schema(item.column_id_))) {
      LOG_INFO("ai column is gone, skip group", K(table_id), K(request_id),
               "column_id", item.column_id_);
      skip_group = true;
    } else {
      ObString expr;
      ObString hint;
      ObSqlString value;
      if (OB_FAIL(ai_worker_split_expr(ai_info->normalized_expr_, expr, hint))) {
      } else if (OB_FAIL(ai_worker_build_value(value, expr, hint, *ai_column))) {
        LOG_WARN("failed to build value expression", K(ret), "column_id", item.column_id_);
      } else {
        if (k > start && OB_FAIL(set_sql.append(", "))) {
        } else if (OB_FAIL(set_sql.append_fmt("`%.*s` = %.*s",
                                              static_cast<int>(item.column_name_.length()),
                                              item.column_name_.ptr(),
                                              static_cast<int>(value.length()),
                                              value.ptr()))) {
        } else {
          max_retries = MAX(max_retries, ai_info->max_retries_);
        }
      }
    }
  }

  if (OB_SUCC(ret) && !skip_group && set_sql.length() > 0) {
    ObString db_name = database_schema->get_database_name_str();
    ObString table_name = table_schema->get_table_name_str();
    if (OB_FAIL(update_sql.append_fmt("update `%.*s`.`%.*s` set ",
                                      static_cast<int>(db_name.length()), db_name.ptr(),
                                      static_cast<int>(table_name.length()), table_name.ptr()))) {
    } else if (OB_FAIL(update_sql.append(set_sql.string()))) {
    } else if (OB_FAIL(update_sql.append_fmt(" where %.*s",
                                             static_cast<int>(predicate.length()),
                                             predicate.ptr()))) {
    } else if (OB_FAIL(ai_worker_exec_write(update_sql.string()))) {
      LOG_WARN("failed to write back ai column values", K(ret), K(table_id),
               K(request_id), KCSTRING(update_sql.ptr()));
    } else if (OB_FAIL(del_sql.assign_fmt(
        "delete from %s where table_id = %lu and request_id = '%.*s'",
        TASK_TABLE, static_cast<unsigned long>(table_id),
        static_cast<int>(request_id.length()), request_id.ptr()))) {
    } else if (OB_FAIL(ai_worker_exec_write(del_sql.string()))) {
      LOG_WARN("failed to remove finished ai column tasks", K(ret));
    } else {
      LOG_INFO("ai_worker_group_done", K(table_id), K(request_id));
    }

    if (OB_FAIL(ret)) {
      // rollback the error code, the retry bookkeeping below decides the fate
      // of this task group.
      ret = OB_SUCCESS;
      int64_t retry_count = tasks.at(start).retry_count_ + 1;
      ObSqlString retry_sql;
      if (retry_count > max_retries) {
        retry_sql.assign_fmt("delete from %s where table_id = %lu and request_id = '%.*s'",
                             TASK_TABLE, static_cast<unsigned long>(table_id),
                             static_cast<int>(request_id.length()), request_id.ptr());
      } else {
        retry_sql.assign_fmt(
            "update %s set state = 3, retry_count = retry_count + 1,"
            " eligible_ts = date_add(now(6), interval 1 second)"
            " where table_id = %lu and request_id = '%.*s'",
            TASK_TABLE, static_cast<unsigned long>(table_id),
            static_cast<int>(request_id.length()), request_id.ptr());
      }
      int tmp_ret = ai_worker_exec_write(retry_sql.string());
      LOG_INFO("ai_worker_group_failed", K(tmp_ret), K(table_id), K(request_id),
               K(retry_count), K(max_retries), KCSTRING(retry_sql.ptr()));
    }
  }

  return ret;
}
} // namespace observer
} // namespace oceanbase
