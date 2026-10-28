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

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "share/ob_define.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace sql
{

enum FileScanFormat
{
  FILE_SCAN_FORMAT_CSV = 0,
  FILE_SCAN_FORMAT_JSONL = 1,
  FILE_SCAN_FORMAT_PARQUET = 2,
};

// Options shared by the resolver (schema inference) and the executor (scan).
struct FileScanOptions
{
  FileScanOptions();
  char field_delim_;
  char line_delim_;
  char quote_;
  char escape_;
  bool has_header_;
  // When set, unquoted values equal to this string are treated as NULL in
  // addition to empty values.
  common::ObString null_str_;
  // -1 means scan the whole file.
  int64_t sample_size_;
};

struct FileScanColumn
{
  FileScanColumn() : name_(), meta_(), accuracy_(), nullable_(true) {}
  common::ObString name_;
  common::ObObjMeta meta_;
  common::ObAccuracy accuracy_;
  bool nullable_;
};

struct FileSnapshot
{
  FileSnapshot()
    : size_(-1), mtime_ns_(0), dev_(0), ino_(0), valid_(false) {}
  int capture(const char *path);
  int check(const char *path) const;
  int64_t size_;
  int64_t mtime_ns_;
  int64_t dev_;
  int64_t ino_;
  bool valid_;
};

// Streaming row reader. Values are kept as text; the executor is responsible
// for casting them to the inferred column types.
class FileScanReader
{
public:
  FileScanReader() : row_count_(0) {}
  virtual ~FileScanReader() {}
  virtual int next_row(bool &eof) = 0;
  const std::vector<std::string> &values() const { return values_; }
  const std::vector<bool> &nulls() const { return nulls_; }
  int64_t row_count() const { return row_count_; }
protected:
  std::vector<std::string> values_;
  std::vector<bool> nulls_;
  int64_t row_count_;
};

// Infer column names/types for the given file. `projection` may be null, in
// which case all columns are described in file order.
int file_scan_infer_schema(const char *path,
                           const FileScanFormat format,
                           const FileScanOptions &options,
                           common::ObIAllocator &allocator,
                           common::ObIArray<FileScanColumn> &columns,
                           int64_t &row_count);

// Create a streaming reader. `projection` contains the file-level column
// indexes that the caller consumes (in output order). When null all columns are
// returned.
int file_scan_create_reader(const char *path,
                            const FileScanFormat format,
                            const FileScanOptions &options,
                            const common::ObIArray<int64_t> *projection,
                            FileScanReader *&reader);

// Enumerate files matching a glob pattern. Only regular files are returned,
// sorted by name.
int file_scan_glob(const char *pattern,
                   common::ObIAllocator &allocator,
                   common::ObIArray<common::ObString> &files);

} // namespace sql
} // namespace oceanbase
