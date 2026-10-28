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

#ifndef OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_
#define OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "lib/charset/ob_charset.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include <cstdio>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{

class ObExpr;
class ObFunctionTableSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(2);
public:
  ObFunctionTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), value_expr_(nullptr), column_exprs_(alloc), column_indexes_(alloc),
      has_correlated_expr_(false), is_file_table_(false), file_table_column_count_(0),
      file_table_size_(0), file_table_mtime_ns_(0), file_table_dev_(0), file_table_ino_(0),
      file_table_header_(true), file_table_format_(0), file_table_field_delim_(','),
      file_table_quote_('"'), file_table_escape_(0), file_table_null_str_()
  {}
  ObExpr *value_expr_;
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  common::ObFixedArray<int64_t, common::ObIAllocator> column_indexes_;
  bool has_correlated_expr_;
  bool is_file_table_;
  int64_t file_table_column_count_;
  int64_t file_table_size_;
  int64_t file_table_mtime_ns_;
  int64_t file_table_dev_;
  int64_t file_table_ino_;
  bool file_table_header_;
  int64_t file_table_format_;
  char file_table_field_delim_;
  char file_table_quote_;
  char file_table_escape_;
  common::ObString file_table_null_str_;
};

class ObFunctionTableOp : public ObOperator
{
public:
  ObFunctionTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
    node_idx_(0),
    already_calc_(false),
    row_count_(0),
    col_count_(0),
    value_table_(NULL),
    file_reader_(nullptr),
    file_snapshot_(),
    file_path_(),
    file_is_glob_(false),
    file_glob_idx_(0)
  {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  //virtual int inner_get_next_batch(int64_t max_row_cnt) override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int inner_get_next_row_udf();
  int inner_get_next_row_sys_func();
  int inner_get_next_row_file();
  int open_file_table();
  int set_file_datum(ObExpr &expr, const std::string &field, bool is_null);
  void close_file_table();
  int get_current_result(common::ObObj &result);
  int64_t node_idx_;
  bool already_calc_;
  int64_t row_count_;
  int64_t col_count_;
  common::ObObj value_;
  pl::ObPLCollection *value_table_;
  int (ObFunctionTableOp::*next_row_func_)();
  FileScanReader *file_reader_;
  FileSnapshot file_snapshot_;
  std::string file_path_;
  bool file_is_glob_;
  size_t file_glob_idx_;
  std::vector<std::string> file_glob_files_;
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_ */
