#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ob_ai_column_dml_helper.h"

#include "share/ob_server_struct.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "lib/string/ob_sql_string.h"
#include "lib/allocator/page_arena.h"
#include "lib/time/ob_time_utility.h"
#include <atomic>
#include <cstdlib>
#include "sql/engine/ob_exec_context.h"
#include "observer/ai_service/ob_ai_column_worker.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share::schema;

namespace
{
const char META_SEP = '\x01';

struct Change
{
  Change() : table_id_(OB_INVALID_ID), event_(0), old_pred_(), new_pred_(), affected_(), request_id_() {}
  uint64_t table_id_;
  int event_;
  ObString old_pred_;
  ObString new_pred_;
  ObString affected_;
  ObString request_id_;
};

struct TlsData
{
  TlsData() : alloc(ObModIds::OB_SQL_EXPR, OB_MALLOC_NORMAL_BLOCK_SIZE), changes() {}
  ObArenaAllocator alloc;
  ObSEArray<Change, 8> changes;
};

TlsData &tls_data()
{
  static thread_local TlsData data;
  return data;
}

bool &in_flush()
{
  static thread_local bool value = false;
  return value;
}

bool &internal_write()
{
  static thread_local bool value = false;
  return value;
}

int write_str(TlsData &tls, const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  if (src.empty()) {
    dst.reset();
  } else {
    char *buf = static_cast<char *>(tls.alloc.alloc(src.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, src.ptr(), src.length());
      dst.assign_ptr(buf, src.length());
    }
  }
  return ret;
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

int append_literal(ObSqlString &sql, const ObDatum &datum, const ObObjMeta &meta)
{
  int ret = OB_SUCCESS;
  if (datum.is_null()) {
    ret = sql.append("NULL");
  } else if (ob_is_string_type(meta.get_type()) || ob_is_json_tc(meta.get_type())) {
    ret = append_escaped(sql, datum.get_string());
  } else if (ob_is_integer_type(meta.get_type())) {
    ret = sql.append_fmt("%ld", datum.get_int());
  } else {
    ObObj obj;
    char buf[256];
    int64_t pos = 0;
    if (OB_FAIL(datum.to_obj(obj, meta))) {
    } else if (OB_FAIL(obj.print_sql_literal(buf, sizeof(buf), pos))) {
    } else if (OB_FAIL(sql.append(buf, pos))) {
    }
  }
  return ret;
}

int get_schema(uint64_t table_id, ObSchemaGetterGuard &guard,
               const ObTableSchema *&table_schema, const ObDatabaseSchema *&db_schema)
{
  int ret = OB_SUCCESS;
  table_schema = nullptr;
  db_schema = nullptr;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(), db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int get_schema(ObTableModifyOp &op, uint64_t table_id,
               const ObTableSchema *&table_schema, const ObDatabaseSchema *&db_schema)
{
  int ret = OB_SUCCESS;
  table_schema = nullptr;
  db_schema = nullptr;
  ObSchemaGetterGuard *guard = nullptr;
  ObExecContext &exec_ctx = op.get_exec_ctx();
  if (OB_NOT_NULL(exec_ctx.get_sql_ctx())) {
    guard = exec_ctx.get_sql_ctx()->schema_guard_;
  }
  if (OB_ISNULL(guard)) {
    ObSchemaGetterGuard local_guard;
    ret = get_schema(table_id, local_guard, table_schema, db_schema);
  } else if (OB_FAIL(guard->get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(guard->get_database_schema(table_schema->get_database_id(), db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

// Access evaluated row values either from a stored row or from an expression row.
struct RowAccessor
{
  RowAccessor() : stored_(nullptr), exprs_(nullptr), eval_ctx_(nullptr), count_(0) {}
  const ObChunkDatumStore::StoredRow *stored_;
  const ExprFixedArray *exprs_;
  ObEvalCtx *eval_ctx_;
  int64_t count_;
};

int row_datum(const RowAccessor &accessor, int64_t idx, const ObDatum *&datum)
{
  int ret = OB_SUCCESS;
  datum = nullptr;
  if (idx < 0 || idx >= accessor.count_) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_NOT_NULL(accessor.stored_)) {
    if (idx >= accessor.stored_->cnt_) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      datum = &accessor.stored_->cells()[idx];
    }
  } else if (OB_NOT_NULL(accessor.exprs_) && OB_NOT_NULL(accessor.eval_ctx_)) {
    ObExpr *expr = accessor.exprs_->at(idx);
    ObDatum *eval_datum = nullptr;
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expr->eval(*accessor.eval_ctx_, eval_datum))) {
    } else {
      datum = eval_datum;
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

int build_predicate(const ObTableSchema &table_schema,
                    const ObIArray<uint64_t> &column_ids,
                    const RowAccessor &row,
                    ObSqlString &pred)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 8> rowkey_ids;
  if (!row.stored_ && !row.exprs_) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(table_schema.get_rowkey_column_ids(rowkey_ids))) {
  } else {
    bool first = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_ids.count(); ++i) {
      int64_t idx = -1;
      for (int64_t j = 0; j < column_ids.count(); ++j) {
        if (column_ids.at(j) == rowkey_ids.at(i)) {
          idx = j;
          break;
        }
      }
      const ObColumnSchemaV2 *col = table_schema.get_column_schema(rowkey_ids.at(i));
      const ObDatum *datum = nullptr;
      if (idx < 0) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_ISNULL(col)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(row_datum(row, idx, datum))) {
      } else if (OB_FAIL(pred.append_fmt("%s`%.*s` = ", first ? "" : " and ",
                                         col->get_column_name_str().length(),
                                         col->get_column_name_str().ptr()))) {
      } else if (OB_FAIL(append_literal(pred, *datum, col->get_meta_type()))) {
      } else {
        first = false;
      }
    }
  }
  return ret;
}

bool same_rowkey(const ObTableSchema &table_schema,
                 const ObIArray<uint64_t> &column_ids,
                 const RowAccessor &old_row,
                 const RowAccessor &new_row)
{
  bool same = true;
  ObSEArray<uint64_t, 8> rowkey_ids;
  if ((!old_row.stored_ && !old_row.exprs_) || (!new_row.stored_ && !new_row.exprs_)
      || OB_SUCCESS != table_schema.get_rowkey_column_ids(rowkey_ids)) {
    same = false;
  } else {
    for (int64_t i = 0; same && i < rowkey_ids.count(); ++i) {
      int64_t idx = -1;
      for (int64_t j = 0; j < column_ids.count(); ++j) {
        if (column_ids.at(j) == rowkey_ids.at(i)) {
          idx = j;
          break;
        }
      }
      const ObDatum *a = nullptr;
      const ObDatum *b = nullptr;
      const ObColumnSchemaV2 *col = table_schema.get_column_schema(rowkey_ids.at(i));
      int tmp_ret = OB_SUCCESS;
      if (idx < 0 || OB_ISNULL(col)) {
        same = false;
      } else if (OB_SUCCESS != (tmp_ret = row_datum(old_row, idx, a))) {
        same = false;
      } else if (OB_SUCCESS != (tmp_ret = row_datum(new_row, idx, b))) {
        same = false;
      } else if (a->is_null() != b->is_null()) {
        same = false;
      } else if (!a->is_null()) {
        if (ob_is_integer_type(col->get_data_type())) {
          same = (a->get_int() == b->get_int());
        } else {
          same = (a->get_string() == b->get_string());
        }
      }
    }
  }
  return same;
}

void split_meta(const ObString &meta, ObString *parts, int64_t &count)
{
  count = 0;
  if (meta.empty()) {
    return;
  }
  const char *begin = meta.ptr();
  const char *end = meta.ptr() + meta.length();
  const char *p = begin;
  while (p <= end && count < 9) {
    const char *sep = p;
    while (sep < end && META_SEP != *sep) {
      ++sep;
    }
    parts[count++].assign_ptr(p, static_cast<int32_t>(sep - p));
    p = sep + 1;
  }
}

int hidden_name(const ObString &base, const char *suffix, ObSqlString &out)
{
  return out.append_fmt("__ai_%.*s_%s", base.length(), base.ptr(), suffix);
}

int append_column_list(ObSqlString &csv, uint64_t column_id, bool &first)
{
  int ret = OB_SUCCESS;
  if (!first && OB_FAIL(csv.append(","))) {
  } else if (OB_FAIL(csv.append_fmt("%lu", column_id))) {
  } else {
    first = false;
  }
  return ret;
}

int record_impl(ObTableModifyOp &op, uint64_t table_id, const ObIArray<uint64_t> &column_ids,
                const RowAccessor &row, ObAiColumnDmlHelper::Event event,
                const RowAccessor &old_row, const ObIArray<uint64_t> *updated_column_ids)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *table_schema = nullptr;
  const ObDatabaseSchema *db_schema = nullptr;
  if (OB_FAIL(get_schema(op, table_id, table_schema, db_schema))) {
    ret = OB_SUCCESS;
  } else if (OB_ISNULL(table_schema) || !table_schema->has_ai_column()) {
    // Fast path: most DML targets tables without AI columns.
  } else {
    TlsData &tls = tls_data();
    Change change;
    change.table_id_ = table_id;
    change.event_ = event;
    ObSqlString affected;
    bool first = true;
    ObSqlString old_pred;
    ObSqlString new_pred;
    bool rowkey_changed = false;
    if (ObAiColumnDmlHelper::AI_EVENT_DELETE == event) {
      if (OB_FAIL(build_predicate(*table_schema, column_ids, row, old_pred))) {
        LOG_WARN("failed to build ai column rowkey predicate", K(ret), K(table_id));
        ret = OB_SUCCESS;
      }
    } else if (ObAiColumnDmlHelper::AI_EVENT_UPDATE == event) {
      if (OB_FAIL(build_predicate(*table_schema, column_ids, row, new_pred))) {
        LOG_WARN("failed to build ai column rowkey predicate", K(ret), K(table_id));
        ret = OB_SUCCESS;
      } else if (!same_rowkey(*table_schema, column_ids, old_row, row)) {
        rowkey_changed = true;
        if (OB_FAIL(build_predicate(*table_schema, column_ids, old_row, old_pred))) {
          LOG_WARN("failed to build old ai column rowkey predicate", K(ret), K(table_id));
          ret = OB_SUCCESS;
        }
      }
    } else {
      if (OB_FAIL(build_predicate(*table_schema, column_ids, row, new_pred))) {
        LOG_WARN("failed to build ai column rowkey predicate", K(ret), K(table_id));
        ret = OB_SUCCESS;
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < table_schema->get_column_count(); ++i) {
      const ObColumnSchemaV2 *ai_col = table_schema->get_column_schema_by_idx(i);
      if (OB_ISNULL(ai_col) || !ai_col->is_ai_column()) {
      } else if (ObAiColumnDmlHelper::AI_EVENT_UPDATE == event && !rowkey_changed) {
        ObSqlString sp_name;
        const ObColumnSchemaV2 *sp_col = nullptr;
        if (OB_FAIL(hidden_name(ai_col->get_column_name_str(), "sp", sp_name))) {
        } else if (OB_ISNULL(sp_col = table_schema->get_column_schema(sp_name.string()))) {
          if (OB_FAIL(append_column_list(affected, ai_col->get_column_id(), first))) {}
        } else {
          ObString parts[9];
          int64_t part_cnt = 0;
          split_meta(sp_col->get_comment_str(), parts, part_cnt);
          bool is_affected = (part_cnt < 3);
          if (!is_affected && updated_column_ids != nullptr) {
            for (int64_t k = 0; !is_affected && k < updated_column_ids->count(); ++k) {
              if (updated_column_ids->at(k) == ai_col->get_column_id()) {
                is_affected = true;
              }
            }
          }
          if (!is_affected && updated_column_ids != nullptr) {
            is_affected = false;
            const char *p = parts[2].ptr();
            const char *end = parts[2].ptr() + parts[2].length();
            while (!is_affected && p < end) {
              char *next = nullptr;
              const int64_t v = strtoll(p, &next, 10);
              if (next == p) {
                break;
              }
              const uint64_t dep = static_cast<uint64_t>(v);
              for (int64_t k = 0; !is_affected && k < updated_column_ids->count(); ++k) {
                is_affected = (updated_column_ids->at(k) == dep);
              }
              p = next;
              while (p < end && (*p == ',' || *p == ' ')) {
                ++p;
              }
            }
          }
          if (is_affected && OB_FAIL(append_column_list(affected, ai_col->get_column_id(), first))) {
          }
        }
      } else if (OB_FAIL(append_column_list(affected, ai_col->get_column_id(), first))) {
      }
    }
    const bool has_pred = !new_pred.string().empty() || !old_pred.string().empty();
    if (OB_SUCC(ret) && !first && has_pred) {
      char req[96];
      static std::atomic<uint64_t> seq(0);
      const uint64_t s = seq.fetch_add(1);
      snprintf(req, sizeof(req), "ai_%ld_%lu", ObTimeUtility::current_time(), s);
      if (OB_FAIL(write_str(tls, old_pred.string(), change.old_pred_))) {
      } else if (OB_FAIL(write_str(tls, new_pred.string(), change.new_pred_))) {
      } else if (OB_FAIL(write_str(tls, affected.string(), change.affected_))) {
      } else if (OB_FAIL(write_str(tls, ObString(static_cast<int32_t>(strlen(req)), req),
                                   change.request_id_))) {
      } else if (OB_FAIL(tls.changes.push_back(change))) {
      }
    }
  }
  return ret;
}

} // namespace

int ObAiColumnDmlHelper::record_row(ObTableModifyOp &op,
                                    uint64_t table_id,
                                    const ObIArray<uint64_t> &column_ids,
                                    const ObChunkDatumStore::StoredRow *row,
                                    Event event,
                                    const ObChunkDatumStore::StoredRow *old_row,
                                    const ObIArray<uint64_t> *updated_column_ids)
{
  int ret = OB_SUCCESS;
  if (in_flush() || internal_write()) {
    return OB_SUCCESS;
  }
  RowAccessor row_accessor;
  row_accessor.stored_ = row;
  row_accessor.count_ = column_ids.count();
  RowAccessor old_accessor;
  old_accessor.stored_ = old_row;
  old_accessor.count_ = column_ids.count();
  if (OB_FAIL(record_impl(op, table_id, column_ids, row_accessor, event,
                          old_accessor, updated_column_ids))) {
  }
  return ret;
}

int ObAiColumnDmlHelper::record_row(ObTableModifyOp &op,
                                    uint64_t table_id,
                                    const ObIArray<uint64_t> &column_ids,
                                    const ExprFixedArray &row,
                                    Event event,
                                    const ExprFixedArray *old_row,
                                    const ObIArray<uint64_t> *updated_column_ids)
{
  int ret = OB_SUCCESS;
  if (in_flush() || internal_write()) {
    return OB_SUCCESS;
  }
  RowAccessor row_accessor;
  row_accessor.exprs_ = &row;
  row_accessor.eval_ctx_ = &op.get_eval_ctx();
  row_accessor.count_ = column_ids.count();
  RowAccessor old_accessor;
  if (OB_NOT_NULL(old_row)) {
    old_accessor.exprs_ = old_row;
    old_accessor.eval_ctx_ = &op.get_eval_ctx();
    old_accessor.count_ = column_ids.count();
  }
  if (OB_FAIL(record_impl(op, table_id, column_ids, row_accessor, event,
                          old_accessor, updated_column_ids))) {
  }
  return ret;
}

void ObAiColumnDmlHelper::set_internal_write(bool value)
{
  internal_write() = value;
}

void ObAiColumnDmlHelper::reset()
{
  if (!in_flush()) {
    TlsData &tls = tls_data();
    tls.changes.reset();
    tls.alloc.reset();
  }
}

int ObAiColumnDmlHelper::flush(ObTableModifyOp &op)
{
  int ret = OB_SUCCESS;
  if (in_flush()) {
    return OB_SUCCESS;
  }
  in_flush() = true;
  ObDASCtx &das_ctx = op.get_exec_ctx().get_das_ctx();
  const bool saved_fk_cascading = das_ctx.is_fk_cascading_;
  das_ctx.is_fk_cascading_ = true;
  TlsData &tls = tls_data();
  uint64_t session_id = 0;
  if (OB_NOT_NULL(op.get_exec_ctx().get_my_session())) {
    session_id = op.get_exec_ctx().get_my_session()->get_server_sid();
  }
  bool conn_opened = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < tls.changes.count(); ++i) {
    const Change &change = tls.changes.at(i);
    const ObTableSchema *table_schema = nullptr;
    const ObDatabaseSchema *db_schema = nullptr;
    if (OB_FAIL(get_schema(op, change.table_id_, table_schema, db_schema))) {
    } else if (AI_EVENT_DELETE == change.event_) {
      ObAiColumnWorker::remove_tasks(change.table_id_, change.old_pred_, ObString());
    } else {
      if (AI_EVENT_UPDATE == change.event_) {
        if (!conn_opened) {
          if (OB_FAIL(op.open_inner_conn())) {
          } else {
            conn_opened = true;
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(op.begin_nested_session(true))) {
        } else if (OB_SUCC(ret)) {
          ObSqlString sql;
          if (OB_FAIL(sql.append_fmt("UPDATE `%.*s`.`%.*s` SET ",
                                     db_schema->get_database_name_str().length(),
                                     db_schema->get_database_name_str().ptr(),
                                     table_schema->get_table_name_str().length(),
                                     table_schema->get_table_name_str().ptr()))) {
          }
          bool first_set = true;
          const char *p = change.affected_.ptr();
          const char *end = change.affected_.ptr() + change.affected_.length();
          while (OB_SUCC(ret) && p < end) {
            char *next = nullptr;
            const uint64_t col_id = strtoull(p, &next, 10);
            const ObColumnSchemaV2 *col = table_schema->get_column_schema(col_id);
            ObSqlString st_name;
            ObSqlString rq_name;
            if (next == p) {
              ret = OB_ERR_UNEXPECTED;
            } else if (OB_ISNULL(col)) {
              ret = OB_ERR_UNEXPECTED;
            } else if (OB_FAIL(hidden_name(col->get_column_name_str(), "st", st_name))) {
            } else if (OB_FAIL(hidden_name(col->get_column_name_str(), "rq", rq_name))) {
            } else if (OB_FAIL(sql.append_fmt("%s`%.*s` = NULL, `%.*s` = 0, `%.*s` = '",
                                              first_set ? "" : ", ",
                                              col->get_column_name_str().length(),
                                              col->get_column_name_str().ptr(),
                                              st_name.string().length(), st_name.string().ptr(),
                                              rq_name.string().length(), rq_name.string().ptr()))) {
            } else if (OB_FAIL(sql.append(change.request_id_))) {
            } else if (OB_FAIL(sql.append("'"))) {
            } else {
              first_set = false;
            }
            p = next;
            while (p < end && (*p == ',' || *p == ' ')) {
              ++p;
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(sql.append(" WHERE "))) {
          } else if (OB_SUCC(ret) && OB_FAIL(sql.append(change.new_pred_))) {
          } else if (OB_SUCC(ret) && OB_FAIL(op.execute_write(sql.ptr()))) {
          }
        }
        int end_ret = OB_SUCCESS;
        if (conn_opened) {
          end_ret = op.end_nested_session();
        }
        if (OB_SUCCESS != end_ret && OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
      if (OB_SUCC(ret) && !change.old_pred_.empty()) {
        ObAiColumnWorker::remove_tasks(change.table_id_, change.old_pred_, ObString());
      }
      const ObString &pred = change.new_pred_.empty() ? change.old_pred_ : change.new_pred_;
      const char *p = change.affected_.ptr();
      const char *end = change.affected_.ptr() + change.affected_.length();
      while (OB_SUCC(ret) && p < end) {
        char *next = nullptr;
        const uint64_t col_id = strtoull(p, &next, 10);
        const ObColumnSchemaV2 *col = table_schema->get_column_schema(col_id);
        ObSqlString sp_name;
        if (next == p) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ISNULL(col)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(hidden_name(col->get_column_name_str(), "sp", sp_name))) {
        } else {
          const ObColumnSchemaV2 *sp_col = table_schema->get_column_schema(sp_name.string());
          ObString parts[9];
          int64_t part_cnt = 0;
          if (OB_NOT_NULL(sp_col)) {
            split_meta(sp_col->get_comment_str(), parts, part_cnt);
          }
          const int64_t max_retries = part_cnt > 0 ? atoll(parts[0].ptr()) : 3;
          const ObString prompt_expr = part_cnt > 4 ? parts[4] : ObString();
          const ObString model = part_cnt > 3 ? parts[3] : ObString();
          const ObString instruction = part_cnt > 5 ? parts[5] : ObString();
          int64_t result_type = 1;
          if (part_cnt > 6 && 0 == parts[6].compare(ObString::make_string("e"))) {
            result_type = 2;
          } else if (ObJsonType == col->get_data_type()) {
            result_type = 0;
          }
          if (OB_FAIL(ObAiColumnWorker::add_task(change.table_id_, pred, col_id,
                                                 change.request_id_, model, prompt_expr,
                                                 instruction, result_type, max_retries,
                                                 session_id))) {
          } else {
            ObAiColumnWorker::mark_incomplete(change.table_id_, col_id);
          }
        }
        p = next;
        while (p < end && (*p == ',' || *p == ' ')) {
          ++p;
        }
      }
    }
  }
  reset();
  das_ctx.is_fk_cascading_ = saved_fk_cascading;
  in_flush() = false;
  return ret;
}

} // namespace sql
} // namespace oceanbase
