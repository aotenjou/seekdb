#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ob_ai_column_worker.h"

#include <pthread.h>
#include <atomic>
#include <cstdlib>
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_se_array.h"
#include "lib/time/ob_time_utility.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/ob_errno.h"
#include "sql/engine/dml/ob_ai_column_dml_helper.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share::schema;

namespace
{
const int64_t MAX_TASK_ENTRIES = 4096;
const int64_t MAX_GROUPS_PER_RUN = 8;
const int64_t RETRY_DELAY_US = 300000;

struct TaskEntry
{
  TaskEntry() : used_(false), table_id_(0), column_id_(0), pred_(nullptr), pred_len_(0),
                request_id_(), state_(0), retry_count_(0), eligible_ts_(0),
                model_(nullptr), model_len_(0), prompt_(nullptr), prompt_len_(0),
                instruction_(nullptr), instruction_len_(0), result_type_(1),
                max_retries_(0), session_id_(0) {}
  bool used_;
  uint64_t table_id_;
  uint64_t column_id_;
  char *pred_;
  int32_t pred_len_;
  char request_id_[64];
  int64_t state_;
  int64_t retry_count_;
  int64_t eligible_ts_;
  char *model_;
  int32_t model_len_;
  char *prompt_;
  int32_t prompt_len_;
  char *instruction_;
  int32_t instruction_len_;
  int64_t result_type_;
  int64_t max_retries_;
  uint64_t session_id_;
};

TaskEntry g_tasks[MAX_TASK_ENTRIES];
pthread_mutex_t g_task_lock = PTHREAD_MUTEX_INITIALIZER;

struct PendingColumn
{
  uint64_t table_id_;
  uint64_t column_id_;
};
const int64_t MAX_PENDING_COLUMNS = 512;
PendingColumn g_pending[MAX_PENDING_COLUMNS];
int64_t g_pending_cnt = 0;
pthread_mutex_t g_pending_lock = PTHREAD_MUTEX_INITIALIZER;

char *dup_string(const ObString &str, int32_t &len)
{
  char *buf = nullptr;
  len = str.length();
  if (len > 0) {
    buf = static_cast<char *>(ob_malloc(len, ObModIds::OB_SQL_EXPR));
    if (OB_NOT_NULL(buf)) {
      MEMCPY(buf, str.ptr(), len);
    }
  }
  return buf;
}

int append_escaped(ObSqlString &sql, const ObString &str)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < str.length(); ++i) {
      const char c = str.ptr()[i];
      if ('\'' == c || '\\' == c) {
        if (OB_FAIL(sql.append(&c, 1))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(sql.append(&str.ptr()[i], 1))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
    }
  }
  return ret;
}

int proxy_write(const char *sql, int64_t &affected)
{
  int ret = OB_SUCCESS;
  affected = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else {
    ObAiColumnDmlHelper::set_internal_write(true);
    ret = GCTX.sql_proxy_->write(sql, affected);
    ObAiColumnDmlHelper::set_internal_write(false);
  }
  return ret;
}

int get_names(uint64_t table_id, ObSqlString &db_name, ObSqlString &table_name)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table_schema = nullptr;
  const ObDatabaseSchema *db_schema = nullptr;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(), db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(db_name.assign(db_schema->get_database_name_str()))) {
  } else if (OB_FAIL(table_name.assign(table_schema->get_table_name_str()))) {
  }
  return ret;
}

int query_column_name(uint64_t table_id, uint64_t column_id, ObSqlString &name)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("SELECT column_name FROM oceanbase.__all_column "
                             "WHERE table_id = %lu AND column_id = %lu", table_id, column_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else {
        ObString str;
        if (OB_FAIL(result->get_varchar(static_cast<const int64_t>(0), str))) {
        } else if (OB_FAIL(name.assign(str))) {
        }
      }
    }
  }
  return ret;
}

int refresh_column_complete(uint64_t table_id, uint64_t column_id)
{
  int ret = OB_SUCCESS;
  ObSqlString db_name;
  ObSqlString table_name;
  ObSqlString col_name;
  if (OB_FAIL(get_names(table_id, db_name, table_name))) {
  } else if (OB_FAIL(query_column_name(table_id, column_id, col_name))) {
  } else {
    ObSqlString sql;
    if (OB_FAIL(sql.append_fmt("SELECT count(*) FROM `%.*s`.`%.*s` WHERE `%.*s` IS NULL",
                               db_name.string().length(), db_name.string().ptr(),
                               table_name.string().length(), table_name.string().ptr(),
                               col_name.string().length(), col_name.string().ptr()))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        sqlclient::ObMySQLResult *result = nullptr;
        if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(result->next())) {
        } else {
          int64_t left = 0;
          if (OB_FAIL(result->get_int(static_cast<const int64_t>(0), left))) {
          } else if (0 == left) {
            ObAiColumnWorker::mark_complete(table_id, column_id);
          }
        }
      }
    }
  }
  return ret;
}

int publish_group(uint64_t table_id, const ObString &pred, const ObString &request_id,
                  const ObIArray<int64_t> &indices, bool &stale)
{
  int ret = OB_SUCCESS;
  stale = false;
  ObSqlString db_name;
  ObSqlString table_name;
  if (indices.empty()) {
    ret = OB_ENTRY_NOT_EXIST;
  } else if (OB_FAIL(get_names(table_id, db_name, table_name))) {
  } else {
    ObSqlString sql;
    if (OB_FAIL(sql.append_fmt("UPDATE `%.*s`.`%.*s` SET ",
                               db_name.string().length(), db_name.string().ptr(),
                               table_name.string().length(), table_name.string().ptr()))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < indices.count(); ++i) {
      const TaskEntry &entry = g_tasks[indices.at(i)];
      ObSqlString col_name;
      const ObString model(entry.model_len_, entry.model_);
      const ObString prompt(entry.prompt_len_, entry.prompt_);
      const ObString instruction(entry.instruction_len_, entry.instruction_);
      if (OB_FAIL(query_column_name(table_id, entry.column_id_, col_name))) {
      } else if (OB_FAIL(sql.append_fmt("%s`%.*s` = ",
                                        i == 0 ? "" : ", ",
                                        col_name.string().length(), col_name.string().ptr()))) {
      } else if (2 == entry.result_type_) {
        if (OB_FAIL(sql.append_fmt("AI_EMBED('%.*s', ", model.length(), model.ptr()))) {
        } else if (OB_FAIL(sql.append(prompt))) {
        } else if (OB_FAIL(sql.append(")"))) {
        }
      } else {
        if (OB_FAIL(sql.append_fmt("AI_COMPLETE('%.*s', ", model.length(), model.ptr()))) {
        } else if (OB_FAIL(sql.append(prompt))) {
        } else if (OB_FAIL(sql.append(", NULL, "))) {
        } else if (OB_FAIL(append_escaped(sql, instruction))) {
        } else if (OB_FAIL(sql.append(")"))) {
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(sql.append_fmt(", `__ai_%.*s_st` = 1, `__ai_%.*s_rq` = '",
                                   col_name.string().length(), col_name.string().ptr(),
                                   col_name.string().length(), col_name.string().ptr()))) {
        } else if (OB_FAIL(sql.append(request_id))) {
        } else if (OB_FAIL(sql.append("'"))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append(" WHERE "))) {
      } else if (OB_FAIL(sql.append(pred))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < indices.count(); ++i) {
        ObSqlString col_name;
        if (OB_FAIL(query_column_name(table_id, g_tasks[indices.at(i)].column_id_, col_name))) {
        } else if (OB_FAIL(sql.append_fmt(" AND (`__ai_%.*s_rq` IS NULL OR `__ai_%.*s_rq` = '",
                                          col_name.string().length(), col_name.string().ptr(),
                                          col_name.string().length(), col_name.string().ptr()))) {
        } else if (OB_FAIL(sql.append(request_id))) {
        } else if (OB_FAIL(sql.append("')"))) {
        }
      }
    }
    int64_t affected = 0;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(proxy_write(sql.ptr(), affected))) {
    } else if (0 == affected) {
      stale = true;
    }
  }
  return ret;
}

int mark_row_failed(uint64_t table_id, const ObString &pred, const ObIArray<int64_t> &indices,
                    const ObString &request_id)
{
  int ret = OB_SUCCESS;
  ObSqlString db_name;
  ObSqlString table_name;
  if (OB_FAIL(get_names(table_id, db_name, table_name))) {
  } else {
    ObSqlString sql;
    if (OB_FAIL(sql.append_fmt("UPDATE `%.*s`.`%.*s` SET ",
                               db_name.string().length(), db_name.string().ptr(),
                               table_name.string().length(), table_name.string().ptr()))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < indices.count(); ++i) {
      ObSqlString col_name;
      if (OB_FAIL(query_column_name(table_id, g_tasks[indices.at(i)].column_id_, col_name))) {
      } else if (OB_FAIL(sql.append_fmt("%s`__ai_%.*s_st` = 2, `__ai_%.*s_rq` = NULL",
                                        i == 0 ? "" : ", ",
                                        col_name.string().length(), col_name.string().ptr(),
                                        col_name.string().length(), col_name.string().ptr()))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append(" WHERE "))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append(pred))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append(" AND `__ai_"))) {
    } else if (OB_SUCC(ret)) {
      ObSqlString first_col;
      if (OB_FAIL(query_column_name(table_id, g_tasks[indices.at(0)].column_id_, first_col))) {
      } else if (OB_FAIL(sql.append_fmt("%.*s_rq` = '", first_col.string().length(),
                                        first_col.string().ptr()))) {
      } else if (OB_FAIL(sql.append(request_id))) {
      } else if (OB_FAIL(sql.append("'"))) {
      }
    }
    int64_t affected = 0;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(proxy_write(sql.ptr(), affected))) {
    }
  }
  return ret;
}

} // namespace

int ObAiColumnWorker::add_task(uint64_t table_id, const ObString &pred, uint64_t column_id,
                               const ObString &request_id, const ObString &model,
                               const ObString &prompt_expr, const ObString &instruction,
                               int64_t result_type, int64_t max_retries, uint64_t session_id)
{
  int ret = OB_SUCCESS;
  pthread_mutex_lock(&g_task_lock);
  int64_t slot = -1;
  for (int64_t i = 0; i < MAX_TASK_ENTRIES; ++i) {
    if (!g_tasks[i].used_) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    TaskEntry &entry = g_tasks[slot];
    entry = TaskEntry();
    entry.used_ = true;
    entry.table_id_ = table_id;
    entry.column_id_ = column_id;
    entry.pred_ = dup_string(pred, entry.pred_len_);
    entry.model_ = dup_string(model, entry.model_len_);
    entry.prompt_ = dup_string(prompt_expr, entry.prompt_len_);
    entry.instruction_ = dup_string(instruction, entry.instruction_len_);
    const int32_t req_len = MIN(request_id.length(), static_cast<int32_t>(sizeof(entry.request_id_) - 1));
    MEMCPY(entry.request_id_, request_id.ptr(), req_len);
    entry.request_id_[req_len] = '\0';
    entry.state_ = 0;
    entry.retry_count_ = 0;
    entry.eligible_ts_ = ObTimeUtility::current_time();
    entry.result_type_ = result_type;
    entry.max_retries_ = max_retries;
    entry.session_id_ = session_id;
  }
  pthread_mutex_unlock(&g_task_lock);
  return ret;
}

void ObAiColumnWorker::remove_tasks(uint64_t table_id, const ObString &pred,
                                    const ObString &request_id)
{
  pthread_mutex_lock(&g_task_lock);
  for (int64_t i = 0; i < MAX_TASK_ENTRIES; ++i) {
    TaskEntry &entry = g_tasks[i];
    if (!entry.used_ || entry.table_id_ != table_id) {
      continue;
    }
    const ObString entry_pred(entry.pred_len_, entry.pred_);
    const ObString entry_req(static_cast<int32_t>(strlen(entry.request_id_)), entry.request_id_);
    if (entry_pred != pred) {
      continue;
    }
    if (!request_id.empty() && entry_req != request_id) {
      continue;
    }
    entry.used_ = false;
  }
  pthread_mutex_unlock(&g_task_lock);
}

void ObAiColumnWorker::remove_session_tasks(uint64_t session_id)
{
  pthread_mutex_lock(&g_task_lock);
  for (int64_t i = 0; i < MAX_TASK_ENTRIES; ++i) {
    if (g_tasks[i].used_ && g_tasks[i].session_id_ == session_id) {
      g_tasks[i].used_ = false;
    }
  }
  pthread_mutex_unlock(&g_task_lock);
}

int ObAiColumnWorker::get_tasks(ObIArray<TaskInfo> &tasks)
{
  int ret = OB_SUCCESS;
  pthread_mutex_lock(&g_task_lock);
  for (int64_t i = 0; OB_SUCC(ret) && i < MAX_TASK_ENTRIES; ++i) {
    TaskEntry &entry = g_tasks[i];
    if (!entry.used_) {
      continue;
    }
    TaskInfo info;
    info.table_id_ = entry.table_id_;
    info.column_id_ = entry.column_id_;
    info.pred_.assign_ptr(entry.pred_, entry.pred_len_);
    info.request_id_.assign_ptr(entry.request_id_, static_cast<int32_t>(strlen(entry.request_id_)));
    info.state_ = entry.state_;
    info.retry_count_ = entry.retry_count_;
    info.eligible_ts_ = entry.eligible_ts_;
    info.model_.assign_ptr(entry.model_, entry.model_len_);
    info.prompt_.assign_ptr(entry.prompt_, entry.prompt_len_);
    info.instruction_.assign_ptr(entry.instruction_, entry.instruction_len_);
    info.result_type_ = entry.result_type_;
    info.max_retries_ = entry.max_retries_;
    info.definition_version_ = 1;
    if (OB_FAIL(tasks.push_back(info))) {
    }
  }
  pthread_mutex_unlock(&g_task_lock);
  return ret;
}

void ObAiColumnWorker::mark_incomplete(uint64_t table_id, uint64_t column_id)
{
  pthread_mutex_lock(&g_pending_lock);
  bool found = false;
  for (int64_t i = 0; i < g_pending_cnt; ++i) {
    if (g_pending[i].table_id_ == table_id && g_pending[i].column_id_ == column_id) {
      found = true;
      break;
    }
  }
  if (!found && g_pending_cnt < MAX_PENDING_COLUMNS) {
    g_pending[g_pending_cnt].table_id_ = table_id;
    g_pending[g_pending_cnt].column_id_ = column_id;
    ++g_pending_cnt;
  }
  pthread_mutex_unlock(&g_pending_lock);
}

void ObAiColumnWorker::mark_complete(uint64_t table_id, uint64_t column_id)
{
  pthread_mutex_lock(&g_pending_lock);
  for (int64_t i = 0; i < g_pending_cnt; ++i) {
    if (g_pending[i].table_id_ == table_id && g_pending[i].column_id_ == column_id) {
      g_pending[i] = g_pending[g_pending_cnt - 1];
      --g_pending_cnt;
      break;
    }
  }
  pthread_mutex_unlock(&g_pending_lock);
}

bool ObAiColumnWorker::is_incomplete(uint64_t table_id, uint64_t column_id)
{
  bool found = false;
  pthread_mutex_lock(&g_pending_lock);
  for (int64_t i = 0; !found && i < g_pending_cnt; ++i) {
    found = (g_pending[i].table_id_ == table_id && g_pending[i].column_id_ == column_id);
  }
  pthread_mutex_unlock(&g_pending_lock);
  return found;
}

bool ObAiColumnWorker::has_unfinished_columns()
{
  bool has = false;
  pthread_mutex_lock(&g_pending_lock);
  has = (g_pending_cnt > 0);
  pthread_mutex_unlock(&g_pending_lock);
  return has;
}

int ObAiColumnWorker::check_ai_column_readable(uint64_t table_id, uint64_t column_id)
{
  int ret = OB_SUCCESS;
  if (is_incomplete(table_id, column_id)) {
    ret = OB_ERR_AI_COLUMN_UNFINISHED;
    LOG_USER_ERROR(OB_ERR_AI_COLUMN_UNFINISHED, table_id, column_id);
  }
  return ret;
}

int ObAiColumnWorker::process_pending()
{
  int ret = OB_SUCCESS;
  static std::atomic<bool> running(false);
  bool expected = false;
  if (!running.compare_exchange_strong(expected, true)) {
    return OB_SUCCESS;
  }
  bool has_task = false;
  pthread_mutex_lock(&g_task_lock);
  for (int64_t i = 0; i < MAX_TASK_ENTRIES; ++i) {
    if (g_tasks[i].used_) {
      has_task = true;
      break;
    }
  }
  pthread_mutex_unlock(&g_task_lock);
  if (!has_task) {
    running = false;
    return OB_SUCCESS;
  }
  for (int64_t round = 0; OB_SUCC(ret) && round < MAX_GROUPS_PER_RUN; ++round) {
    ObSEArray<int64_t, 8> indices;
    uint64_t table_id = 0;
    bool found = false;
    pthread_mutex_lock(&g_task_lock);
    for (int64_t i = 0; i < MAX_TASK_ENTRIES; ++i) {
      TaskEntry &entry = g_tasks[i];
      if (entry.used_ && (0 == entry.state_ || 3 == entry.state_)
          && entry.eligible_ts_ <= ObTimeUtility::current_time()) {
        table_id = entry.table_id_;
        found = true;
        break;
      }
    }
    if (found) {
      const TaskEntry &first = g_tasks[0];
      UNUSED(first);
      ObString pred;
      ObString request_id;
      for (int64_t i = 0; i < MAX_TASK_ENTRIES; ++i) {
        TaskEntry &entry = g_tasks[i];
        if (!entry.used_ || entry.table_id_ != table_id
            || (0 != entry.state_ && 3 != entry.state_)
            || entry.eligible_ts_ > ObTimeUtility::current_time()) {
          continue;
        }
        if (indices.empty()) {
          pred.assign_ptr(entry.pred_, entry.pred_len_);
          request_id.assign_ptr(entry.request_id_, static_cast<int32_t>(strlen(entry.request_id_)));
        } else {
          const ObString entry_pred(entry.pred_len_, entry.pred_);
          const ObString entry_req(static_cast<int32_t>(strlen(entry.request_id_)), entry.request_id_);
          if (entry_pred != pred || entry_req != request_id) {
            continue;
          }
        }
        entry.state_ = 1;
        ++entry.retry_count_;
        indices.push_back(i);
      }
    }
    pthread_mutex_unlock(&g_task_lock);
    if (!found) {
      break;
    }
    if (indices.empty()) {
      continue;
    }
    TaskEntry &first_entry = g_tasks[indices.at(0)];
    const ObString pred(first_entry.pred_len_, first_entry.pred_);
    const ObString request_id(static_cast<int32_t>(strlen(first_entry.request_id_)),
                              first_entry.request_id_);
    bool stale = false;
    int publish_ret = publish_group(table_id, pred, request_id, indices, stale);
    if (OB_SUCCESS == publish_ret && !stale) {
      ObAiColumnWorker::remove_tasks(table_id, pred, request_id);
      for (int64_t k = 0; k < indices.count(); ++k) {
        int tmp = refresh_column_complete(table_id, g_tasks[indices.at(k)].column_id_);
        UNUSED(tmp);
      }
    } else if (OB_SUCCESS == publish_ret && stale) {
      ObAiColumnWorker::remove_tasks(table_id, pred, request_id);
    } else {
      LOG_WARN("ai column publish failed", K(publish_ret));
      bool terminal = false;
      for (int64_t k = 0; k < indices.count(); ++k) {
        if (g_tasks[indices.at(k)].retry_count_ > g_tasks[indices.at(k)].max_retries_) {
          terminal = true;
        }
      }
      if (terminal) {
        int tmp = mark_row_failed(table_id, pred, indices, request_id);
        UNUSED(tmp);
        ObAiColumnWorker::remove_tasks(table_id, pred, request_id);
      } else {
        pthread_mutex_lock(&g_task_lock);
        for (int64_t k = 0; k < indices.count(); ++k) {
          g_tasks[indices.at(k)].state_ = 3;
          g_tasks[indices.at(k)].eligible_ts_ = ObTimeUtility::current_time() + RETRY_DELAY_US;
        }
        pthread_mutex_unlock(&g_task_lock);
      }
    }
  }
  running = false;
  return ret;
}

} // namespace sql
} // namespace oceanbase
