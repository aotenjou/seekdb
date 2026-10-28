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

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/expr/ob_obj_cast_runtime.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "share/object/ob_obj_cast.h"
#include <sys/stat.h>
#include <limits.h>
#include <cstring>


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, column_indexes_,
                    has_correlated_expr_, is_file_table_, file_table_column_count_, file_table_size_,
                    file_table_mtime_ns_, file_table_dev_, file_table_ino_, file_table_header_,
                    file_table_format_, file_table_field_delim_, file_table_quote_,
                    file_table_escape_, file_table_null_str_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (MY_SPEC.is_file_table_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file;
    if (OB_FAIL(open_file_table())) {
    }
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.is_file_table_) {
      close_file_table();
      if (OB_FAIL(open_file_table())) {
      }
    } else if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  close_file_table();
  return ret;
}

void ObFunctionTableOp::destroy()
{
  close_file_table();
  ObOperator::destroy();
}

void ObFunctionTableOp::close_file_table()
{
  if (OB_NOT_NULL(file_reader_)) {
    delete file_reader_;
    file_reader_ = nullptr;
  }
  file_snapshot_ = FileSnapshot();
  file_path_.clear();
  file_is_glob_ = false;
  file_glob_idx_ = 0;
  file_glob_files_.clear();
}

int ObFunctionTableOp::open_file_table()
{
  int ret = OB_SUCCESS;
  ObDatum *path_datum = nullptr;
  ObString path;
  ObString secure_file_priv;
  ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
  char canonical_path[PATH_MAX + 1] = {0};
  char *resolved_path = nullptr;
  const bool is_glob = (FILE_TABLE_GLOB == MY_SPEC.file_table_format_);
  std::string path_str;
  bool enabled = false;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL is disabled; set enable_file_sql = ON");
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, path_datum))) {
  } else if (OB_ISNULL(path_datum) || path_datum->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (FALSE_IT(path = path_datum->get_string())) {
  } else if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (FALSE_IT(path_str.assign(path.ptr(), path.length()))) {
  } else if (is_glob) {
    if (OB_FAIL(session->get_secure_file_priv(secure_file_priv))) {
    } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv, path))) {
    } else {
      resolved_path = path_str.data();
    }
  } else if (OB_ISNULL(resolved_path = realpath(path_str.c_str(), canonical_path))) {
    ret = OB_FILE_NOT_EXIST;
  } else if (OB_FAIL(session->get_secure_file_priv(secure_file_priv))) {
  } else if (OB_FAIL(ObResolverUtils::check_secure_path(
                 secure_file_priv,
                 ObString(static_cast<int32_t>(strlen(resolved_path)), resolved_path)))) {
  } else if (FALSE_IT(file_snapshot_.valid_ = true)
             || FALSE_IT(file_snapshot_.size_ = MY_SPEC.file_table_size_)
             || FALSE_IT(file_snapshot_.mtime_ns_ = MY_SPEC.file_table_mtime_ns_)
             || FALSE_IT(file_snapshot_.dev_ = MY_SPEC.file_table_dev_)
             || FALSE_IT(file_snapshot_.ino_ = MY_SPEC.file_table_ino_)) {
  } else if (OB_FAIL(file_snapshot_.check(resolved_path))) {
    if (OB_STATE_NOT_MATCH == ret) {
      LOG_USER_ERROR(OB_STATE_NOT_MATCH, "File changed after query planning");
    }
  }
  if (OB_SUCC(ret)) {
    FileScanOptions options;
    FileScanFormat format = FILE_SCAN_FORMAT_CSV;
    if (is_glob) {
      format = FILE_SCAN_FORMAT_CSV;
    } else {
      if (FILE_TABLE_JSON == MY_SPEC.file_table_format_) {
        format = FILE_SCAN_FORMAT_JSONL;
      } else if (FILE_TABLE_PARQUET == MY_SPEC.file_table_format_) {
        format = FILE_SCAN_FORMAT_PARQUET;
      }
    }
    options.field_delim_ = MY_SPEC.file_table_field_delim_;
    options.quote_ = MY_SPEC.file_table_quote_;
    options.escape_ = MY_SPEC.file_table_escape_;
    options.has_header_ = MY_SPEC.file_table_header_;
    options.null_str_ = MY_SPEC.file_table_null_str_;
    if (is_glob) {
      ObSEArray<ObString, 16> files;
      if (OB_FAIL(file_scan_glob(resolved_path, ctx_.get_allocator(), files))) {
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < files.count(); ++i) {
          file_glob_files_.push_back(std::string(files.at(i).ptr(), files.at(i).length()));
        }
        file_is_glob_ = true;
        file_path_.assign(resolved_path);
      }
    } else if (OB_FAIL(file_scan_create_reader(resolved_path, format, options,
                                               &MY_SPEC.column_indexes_, file_reader_))) {
    } else {
      file_path_.assign(resolved_path);
    }
  }
  if (OB_FAIL(ret)) {
    close_file_table();
  }
  return ret;
}

int ObFunctionTableOp::set_file_datum(ObExpr &expr, const std::string &field, bool is_null)
{
  int ret = OB_SUCCESS;
  ObDatum &dst_datum = expr.locate_datum_for_write(eval_ctx_);
  if (is_null) {
    dst_datum.set_null();
  } else {
    ObObj src_obj;
    ObObj dst_obj;
    if (ObTinyIntType == expr.obj_meta_.get_type()
        && (ObCharset::case_insensitive_equal(
              ObString(static_cast<int32_t>(field.size()), const_cast<char *>(field.data())),
              ObString::make_string("true"))
            || ObCharset::case_insensitive_equal(
              ObString(static_cast<int32_t>(field.size()), const_cast<char *>(field.data())),
              ObString::make_string("false")))) {
      src_obj.set_varchar(('t' == tolower(static_cast<unsigned char>(field[0])))
                          ? "1" : "0", 1);
    } else {
      src_obj.set_varchar(field.data(), static_cast<int32_t>(field.size()));
    }
    src_obj.set_collation_type(expr.obj_meta_.get_collation_type());
    const ObDataTypeCastParams dtc_params = ObBasicSessionInfo::create_dtc_params(GET_MY_SESSION(ctx_));
    ObAccuracy dst_accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY2[0][expr.obj_meta_.get_type()];
    ObCastCtx cast_ctx(&eval_ctx_.exec_ctx_.get_allocator(), &dtc_params, CM_WARN_ON_FAIL,
                       expr.obj_meta_.get_collation_type(), &dst_accuracy);
    ObSqlObjCastRuntime cast_runtime(&eval_ctx_.exec_ctx_);
    cast_ctx.runtime_ = &cast_runtime;
    if (OB_FAIL(ObObjCaster::to_type(expr.obj_meta_.get_type(), expr.obj_meta_.get_collation_type(),
                                   cast_ctx, src_obj, dst_obj))) {
    } else if (OB_FAIL(dst_datum.from_obj(dst_obj, expr.obj_datum_map_))) {
    } else if (is_lob_storage(dst_obj.get_type()) &&
               OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), dst_obj, expr.obj_meta_,
                                           expr.obj_datum_map_, get_exec_ctx().get_allocator(),
                                           dst_datum))) {
    }
  }
  if (OB_SUCC(ret)) {
    expr.set_evaluated_projected(eval_ctx_);
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_file()
{
  int ret = OB_SUCCESS;
  bool eof = false;
  clear_evaluated_flag();
  if (OB_FAIL(ctx_.check_status())) {
  } else if (file_is_glob_) {
    if (file_glob_idx_ >= file_glob_files_.size()) {
      ret = OB_ITER_END;
    } else if (MY_SPEC.column_exprs_.count() != 1) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      OZ(set_file_datum(*MY_SPEC.column_exprs_.at(0), file_glob_files_[file_glob_idx_], false));
      ++file_glob_idx_;
    }
  } else if (OB_ISNULL(file_reader_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(file_reader_->next_row(eof))) {
  } else if (eof) {
    ret = OB_ITER_END;
  } else if (MY_SPEC.column_indexes_.count() != MY_SPEC.column_exprs_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const std::vector<std::string> &values = file_reader_->values();
    const std::vector<bool> &nulls = file_reader_->nulls();
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      if (i >= static_cast<int64_t>(values.size())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        OZ(set_file_datum(*MY_SPEC.column_exprs_.at(i), values[i], nulls[i]));
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
