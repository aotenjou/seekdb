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

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <new>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "common/timezone/ob_time_convert.h"
#include "lib/charset/ob_charset.h"
#include "lib/utility/ob_macro_utils.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{

namespace
{

enum FsLogicalType
{
  ST_NULL = 0,
  ST_BOOL,
  ST_INT,
  ST_DOUBLE,
  ST_VARCHAR,
  ST_DATE,
  ST_DATETIME,
};

struct FsColumnState
{
  FsColumnState() : name_(), type_(ST_NULL), nullable_(true) {}
  std::string name_;
  FsLogicalType type_;
  bool nullable_;
};

const char *fs_scan_type_name(const FsLogicalType type)
{
  switch (type) {
    case ST_BOOL: return "BOOLEAN";
    case ST_INT: return "BIGINT";
    case ST_DOUBLE: return "DOUBLE";
    case ST_VARCHAR: return "VARCHAR";
    case ST_DATE: return "DATE";
    case ST_DATETIME: return "DATETIME";
    default: return "VARCHAR";
  }
}

void fs_scan_type_to_meta(const FsLogicalType type, common::ObObjMeta &meta, common::ObAccuracy &accuracy)
{
  switch (type) {
    case ST_BOOL:
      meta.set_type(ObTinyIntType);
      accuracy = ObAccuracy(1, 0);
      break;
    case ST_INT:
      meta.set_type(ObIntType);
      accuracy = ObAccuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObIntType].get_precision(), 0);
      break;
    case ST_DOUBLE:
      meta.set_type(ObDoubleType);
      accuracy = ObAccuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObDoubleType].get_precision(), SCALE_UNKNOWN_YET);
      break;
    case ST_DATE:
      meta.set_type(ObDateType);
      accuracy = ObAccuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObDateType].get_precision(), 0);
      break;
    case ST_DATETIME:
      meta.set_type(ObDateTimeType);
      accuracy = ObAccuracy(ObAccuracy::MAX_ACCURACY2[0][ObDateTimeType].get_precision(), 0);
      break;
    case ST_VARCHAR:
    default:
      meta.set_type(ObVarcharType);
      accuracy = ObAccuracy(PRECISION_UNKNOWN_YET, SCALE_UNKNOWN_YET);
      break;
  }
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  meta.set_collation_level(CS_LEVEL_IMPLICIT);
}

bool fs_iequal(const std::string &lhs, const char *rhs)
{
  const size_t rhs_len = strlen(rhs);
  if (lhs.size() != rhs_len) {
    return false;
  }
  for (size_t i = 0; i < rhs_len; ++i) {
    if (tolower(static_cast<unsigned char>(lhs[i])) != tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

bool fs_parse_int_strict(const std::string &value, int64_t &result)
{
  bool ok = false;
  if (!value.empty()) {
    char *end = nullptr;
    errno = 0;
    const long long parsed = strtoll(value.c_str(), &end, 10);
    if (0 == errno && nullptr != end && end == value.c_str() + value.size()) {
      result = static_cast<int64_t>(parsed);
      ok = true;
    }
  }
  return ok;
}

bool fs_parse_double_strict(const std::string &value, double &result)
{
  bool ok = false;
  if (!value.empty()) {
    char *end = nullptr;
    errno = 0;
    const double parsed = strtod(value.c_str(), &end);
    if (0 == errno && nullptr != end && end == value.c_str() + value.size() && std::isfinite(parsed)) {
      result = parsed;
      ok = true;
    }
  }
  return ok;
}

bool fs_is_date_text(const std::string &value)
{
  bool is_date = false;
  if (10 == value.size() && '-' == value[4] && '-' == value[7]) {
    int32_t date = 0;
    if (OB_SUCCESS == ObTimeConverter::str_to_date(
            ObString(static_cast<int32_t>(value.size()), const_cast<char *>(value.data())), date)) {
      is_date = true;
    }
  }
  return is_date;
}

bool fs_is_datetime_text(const std::string &value)
{
  bool is_datetime = false;
  if (value.size() >= 19 && (' ' == value[10] || 'T' == value[10])) {
    ObTimeConvertCtx ctx(nullptr, false);
    int64_t datetime = 0;
    if (OB_SUCCESS == ObTimeConverter::str_to_datetime(
            ObString(static_cast<int32_t>(value.size()), const_cast<char *>(value.data())),
            ctx, datetime)) {
      is_datetime = true;
    }
  }
  return is_datetime;
}

bool fs_is_bool_text(const std::string &value)
{
  return fs_iequal(value, "true") || fs_iequal(value, "false");
}

FsLogicalType fs_widen_type(const FsLogicalType current, const FsLogicalType incoming)
{
  FsLogicalType result = current;
  if (ST_NULL == incoming) {
    result = current;
  } else if (ST_NULL == current) {
    result = incoming;
  } else if (current == incoming) {
    result = current;
  } else if ((ST_BOOL == current && ST_INT == incoming)
             || (ST_INT == current && ST_BOOL == incoming)) {
    result = ST_INT;
  } else if ((ST_BOOL == current && ST_DOUBLE == incoming)
             || (ST_DOUBLE == current && ST_BOOL == incoming)
             || (ST_INT == current && ST_DOUBLE == incoming)
             || (ST_DOUBLE == current && ST_INT == incoming)) {
    result = ST_DOUBLE;
  } else {
    result = ST_VARCHAR;
  }
  return result;
}

FsLogicalType fs_classify_text(const std::string &value)
{
  FsLogicalType type = ST_VARCHAR;
  int64_t int_value = 0;
  double double_value = 0;
  if (fs_is_bool_text(value)) {
    type = ST_BOOL;
  } else if (fs_is_date_text(value)) {
    type = ST_DATE;
  } else if (fs_is_datetime_text(value)) {
    type = ST_DATETIME;
  } else if (fs_parse_int_strict(value, int_value)) {
    type = ST_INT;
  } else if (fs_parse_double_strict(value, double_value)) {
    type = ST_DOUBLE;
  }
  return type;
}

void fs_classify_json_scalar(ObJsonNode *node, FsLogicalType &type, std::string &text, bool &is_null)
{
  type = ST_VARCHAR;
  is_null = false;
  text.clear();
  if (OB_ISNULL(node)) {
    is_null = true;
  } else {
    switch (node->json_type()) {
      case ObJsonNodeType::J_NULL:
        is_null = true;
        break;
      case ObJsonNodeType::J_BOOLEAN:
        type = ST_BOOL;
        text = node->get_boolean() ? "true" : "false";
        break;
      case ObJsonNodeType::J_INT:
        type = ST_INT;
        text = std::to_string(node->get_int());
        break;
      case ObJsonNodeType::J_UINT:
        type = ST_INT;
        text = std::to_string(node->get_uint());
        break;
      case ObJsonNodeType::J_DOUBLE:
      case ObJsonNodeType::J_DECIMAL: {
        type = ST_DOUBLE;
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", node->get_double());
        text.assign(buf);
        break;
      }
      case ObJsonNodeType::J_STRING: {
        ObString str = static_cast<ObJsonString *>(node)->get_str();
        text.assign(str.ptr(), str.length());
        if (fs_is_date_text(text)) {
          type = ST_DATE;
        } else if (fs_is_datetime_text(text)) {
          type = ST_DATETIME;
        }
        break;
      }
      default:
        type = ST_VARCHAR;
        break;
    }
  }
}

int fs_add_or_update_column(std::vector<FsColumnState> &columns,
                         const std::string &name,
                         const FsLogicalType incoming,
                         const bool is_null)
{
  int ret = OB_SUCCESS;
  int64_t found = -1;
  for (int64_t i = 0; i < static_cast<int64_t>(columns.size()); ++i) {
    if (fs_iequal(columns[i].name_, name.c_str())) {
      found = i;
      break;
    }
  }
  if (found < 0) {
    FsColumnState column;
    if (name.empty()) {
      column.name_ = "column_" + std::to_string(columns.size() + 1);
    } else {
      column.name_ = name;
    }
    column.type_ = is_null ? ST_NULL : incoming;
    column.nullable_ = is_null;
    columns.push_back(column);
  } else {
    FsColumnState &column = columns[found];
    column.type_ = fs_widen_type(column.type_, incoming);
    column.nullable_ = column.nullable_ || is_null;
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// CSV

struct FsCsvParseState
{
  FsCsvParseState() : fields(), nulls(), quoted() {}
  std::vector<std::string> fields;
  std::vector<bool> nulls;
  std::vector<bool> quoted;
};

class FsCsvScanner
{
public:
  FsCsvScanner() : file_(nullptr), options_() {}
  ~FsCsvScanner() { close(); }
  int open(const char *path, const FileScanOptions &options)
  {
    int ret = OB_SUCCESS;
    options_ = options;
    if (OB_ISNULL(path)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_ISNULL(file_ = fopen(path, "rb"))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open csv file", K(ret), K(path), K(errno));
    } else {
      // skip utf-8 BOM
      int c1 = fgetc(file_);
      int c2 = fgetc(file_);
      int c3 = fgetc(file_);
      if (!(0xEF == c1 && 0xBB == c2 && 0xBF == c3)) {
        if (EOF != c3) {
          ungetc(c3, file_);
        }
        if (EOF != c2) {
          ungetc(c2, file_);
        }
        if (EOF != c1) {
          ungetc(c1, file_);
        }
      }
    }
    return ret;
  }
  void close()
  {
    if (OB_NOT_NULL(file_)) {
      fclose(file_);
      file_ = nullptr;
    }
  }
  FILE *file() const { return file_; }
  const FileScanOptions &options() const { return options_; }
  int read_record(FsCsvParseState &state, bool &eof);
private:
  FILE *file_;
  FileScanOptions options_;
};

int FsCsvScanner::read_record(FsCsvParseState &state, bool &eof)
{
  int ret = OB_SUCCESS;
  state.fields.clear();
  state.nulls.clear();
  state.quoted.clear();
  eof = false;
  std::string field;
  bool in_quotes = false;
  bool field_quoted = false;
  bool saw_data = false;
  bool field_finished = false;
  int ch = 0;
  if (OB_ISNULL(file_)) {
    ret = OB_ERR_UNEXPECTED;
  }
  while (OB_SUCC(ret) && EOF != (ch = fgetc(file_))) {
    saw_data = true;
    if (in_quotes) {
      if (options_.escape_ != 0 && ch == static_cast<unsigned char>(options_.escape_)) {
        const int next = fgetc(file_);
        if (EOF == next) {
          ret = OB_ERR_PARSER_SYNTAX;
          LOG_WARN("unexpected end of file after escape character", K(ret));
        } else if (next == static_cast<unsigned char>(options_.quote_)
                   || next == static_cast<unsigned char>(options_.escape_)) {
          field.push_back(static_cast<char>(next));
        } else {
          field.push_back(static_cast<char>(ch));
          ungetc(next, file_);
        }
      } else if (ch == static_cast<unsigned char>(options_.quote_)) {
        const int next = fgetc(file_);
        if (next == static_cast<unsigned char>(options_.quote_)) {
          field.push_back(static_cast<char>(options_.quote_));
        } else {
          in_quotes = false;
          if (EOF != next) {
            ungetc(next, file_);
          }
        }
      } else {
        field.push_back(static_cast<char>(ch));
      }
      field_finished = false;
    } else if (ch == static_cast<unsigned char>(options_.quote_) && field.empty() && !field_quoted) {
      in_quotes = true;
      field_quoted = true;
      field_finished = false;
    } else if (ch == static_cast<unsigned char>(options_.field_delim_)) {
      state.fields.push_back(field);
      state.quoted.push_back(field_quoted);
      field.clear();
      field_quoted = false;
      field_finished = true;
    } else if (ch == '\n' || ch == static_cast<unsigned char>(options_.line_delim_)) {
      if (!field.empty() && '\r' == field.back()) {
        field.pop_back();
      }
      state.fields.push_back(field);
      state.quoted.push_back(field_quoted);
      field.clear();
      field_finished = true;
      break;
    } else if ('\r' == ch) {
      const int next = fgetc(file_);
      if (EOF != next && '\n' != next) {
        ungetc(next, file_);
      } else if (EOF == next) {
        // last line ends with CR
      }
      if (!field.empty() && '\r' == field.back()) {
        field.pop_back();
      }
      state.fields.push_back(field);
      state.quoted.push_back(field_quoted);
      field.clear();
      field_finished = true;
      break;
    } else {
      field.push_back(static_cast<char>(ch));
      field_finished = false;
    }
  }
  if (OB_SUCC(ret)) {
    if (in_quotes) {
      ret = OB_ERR_PARSER_SYNTAX;
      LOG_WARN("unterminated quoted field in csv file", K(ret));
    } else if (!saw_data) {
      eof = true;
    } else if (!field_finished) {
      if (!field.empty() && '\r' == field.back()) {
        field.pop_back();
      }
      state.fields.push_back(field);
      state.quoted.push_back(field_quoted);
      eof = true;
    }
  }
  if (OB_SUCC(ret)) {
    state.nulls.resize(state.fields.size(), false);
    for (size_t i = 0; i < state.fields.size(); ++i) {
      bool is_null = false;
      if (!state.quoted[i]) {
        const std::string &value = state.fields[i];
        if (value.empty()) {
          is_null = true;
        } else if (!options_.null_str_.empty()) {
          is_null = (value.size() == static_cast<size_t>(options_.null_str_.length())
                     && 0 == strncasecmp(value.c_str(), options_.null_str_.ptr(), value.size()));
        } else {
          is_null = fs_iequal(value, "NULL");
        }
      }
      state.nulls[i] = is_null;
    }
  }
  return ret;
}

int fs_infer_csv(const char *path, const FileScanOptions &options,
              std::vector<FsColumnState> &columns, int64_t &row_count)
{
  int ret = OB_SUCCESS;
  row_count = 0;
  FsCsvScanner scanner;
  if (OB_FAIL(scanner.open(path, options))) {
  } else {
    FsCsvParseState state;
    bool eof = false;
    if (OB_FAIL(scanner.read_record(state, eof))) {
    } else if (!eof && options.has_header_) {
      for (int64_t i = 0; i < static_cast<int64_t>(state.fields.size()); ++i) {
        std::string name = state.fields[i];
        if (name.empty()) {
          name = "column_" + std::to_string(i + 1);
        }
        int64_t duplicate_count = 0;
        for (int64_t j = 0; j < static_cast<int64_t>(columns.size()); ++j) {
          if (fs_iequal(columns[j].name_, name.c_str())) {
            ++duplicate_count;
          }
        }
        if (duplicate_count > 0) {
          name += "__" + std::to_string(duplicate_count + 1);
        }
        FsColumnState column;
        column.name_ = name;
        column.type_ = ST_NULL;
        column.nullable_ = false;
        columns.push_back(column);
      }
      if (OB_FAIL(scanner.read_record(state, eof))) {
      }
    }
    while (OB_SUCC(ret) && !eof) {
      if (static_cast<int64_t>(state.fields.size()) != static_cast<int64_t>(columns.size())) {
        if (columns.empty() && !options.has_header_) {
          for (int64_t i = 0; i < static_cast<int64_t>(state.fields.size()); ++i) {
            FsColumnState column;
            column.name_ = "column_" + std::to_string(i + 1);
            column.type_ = ST_NULL;
            column.nullable_ = true;
            columns.push_back(column);
          }
        } else {
          ret = OB_INVALID_DATA;
          LOG_WARN("csv row has different field count", K(ret), K(row_count),
                   K(state.fields.size()), K(columns.size()));
        }
      }
      if (OB_SUCC(ret)) {
        for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(columns.size()); ++i) {
          FsColumnState &column = columns[i];
          if (state.nulls[i]) {
            column.nullable_ = true;
          } else {
            column.type_ = fs_widen_type(column.type_, fs_classify_text(state.fields[i]));
          }
        }
        ++row_count;
      }
      if (OB_SUCC(ret)) {
        if (options.sample_size_ > 0 && row_count >= options.sample_size_) {
          break;
        } else if (OB_FAIL(scanner.read_record(state, eof))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; i < static_cast<int64_t>(columns.size()); ++i) {
      if (ST_NULL == columns[i].type_) {
        columns[i].type_ = ST_VARCHAR;
        columns[i].nullable_ = true;
      }
    }
  }
  return ret;
}

class FsCsvFileReader : public FileScanReader
{
public:
  FsCsvFileReader() : scanner_() {}
  virtual ~FsCsvFileReader() {}
  int init(const char *path, const FileScanOptions &options)
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(scanner_.open(path, options))) {
    } else if (options.has_header_) {
      FsCsvParseState header;
      bool eof = false;
      if (OB_FAIL(scanner_.read_record(header, eof))) {
      } else if (eof) {
        ret = OB_INVALID_DATA;
        LOG_WARN("csv file only contains a header", K(ret));
      }
    }
    return ret;
  }
  virtual int next_row(bool &eof) override
  {
    int ret = OB_SUCCESS;
    FsCsvParseState state;
    if (OB_FAIL(scanner_.read_record(state, eof))) {
    } else if (eof) {
    } else {
      values_ = state.fields;
      nulls_ = state.nulls;
      ++row_count_;
    }
    return ret;
  }
private:
  FsCsvScanner scanner_;
};

////////////////////////////////////////////////////////////////////////////////
// JSON lines

int fs_read_logical_line(FILE *file, std::string &line, bool &eof)
{
  int ret = OB_SUCCESS;
  line.clear();
  eof = false;
  int ch = 0;
  while (EOF != (ch = fgetc(file))) {
    if ('\n' == ch) {
      break;
    } else if ('\r' == ch) {
      const int next = fgetc(file);
      if (EOF != next && '\n' != next) {
        ungetc(next, file);
      }
      break;
    } else {
      line.push_back(static_cast<char>(ch));
    }
  }
  if (EOF == ch && line.empty()) {
    eof = true;
  }
  return ret;
}

int fs_parse_json_line(ObIAllocator &allocator, const std::string &line, ObJsonObject *&object)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;
  object = nullptr;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, line.data(),
                                               static_cast<uint64_t>(line.size()),
                                               ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE,
                                               base))) {
  } else if (OB_ISNULL(base) || base->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
    LOG_WARN("json line is not a json object", K(ret));
  } else {
    object = static_cast<ObJsonObject *>(base);
  }
  return ret;
}

int fs_infer_jsonl(const char *path, std::vector<FsColumnState> &columns, int64_t &row_count)
{
  int ret = OB_SUCCESS;
  row_count = 0;
  FILE *file = nullptr;
  if (OB_ISNULL(path) || OB_ISNULL(file = fopen(path, "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("failed to open jsonl file", K(ret), K(path), K(errno));
  } else {
    std::string line;
    bool eof = false;
    ObArenaAllocator arena;
    while (OB_SUCC(ret) && !eof) {
      if (OB_FAIL(fs_read_logical_line(file, line, eof))) {
      } else if (eof) {
        break;
      } else if (line.empty()) {
        continue;
      } else {
        arena.reset();
        ObJsonObject *object = nullptr;
        if (OB_FAIL(fs_parse_json_line(arena, line, object))) {
        } else {
          const uint64_t count = object->element_count();
          for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
            ObString key;
            ObJsonNode *value = nullptr;
            if (OB_FAIL(object->get_value_by_idx(i, key, value))) {
            } else if (OB_ISNULL(value)) {
              ret = OB_ERR_UNEXPECTED;
            } else if (value->json_type() == ObJsonNodeType::J_OBJECT
                       || value->json_type() == ObJsonNodeType::J_ARRAY) {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested json value in read_json");
            } else {
              FsLogicalType type = ST_VARCHAR;
              std::string text;
              bool is_null = false;
              fs_classify_json_scalar(value, type, text, is_null);
              std::string column_name(key.ptr(), key.length());
              if (OB_FAIL(fs_add_or_update_column(columns, column_name, type, is_null))) {
              }
            }
          }
          ++row_count;
        }
      }
    }
    if (OB_NOT_NULL(file)) {
      fclose(file);
      file = nullptr;
    }
  }
  if (OB_SUCC(ret)) {
    // fields missing from some rows make the union schema nullable
    for (int64_t i = 0; i < static_cast<int64_t>(columns.size()); ++i) {
      if (ST_NULL == columns[i].type_) {
        columns[i].type_ = ST_VARCHAR;
        columns[i].nullable_ = true;
      }
    }
  }
  return ret;
}

class FsJsonlFileReader : public FileScanReader
{
public:
  FsJsonlFileReader() : file_(nullptr), file_path_(), columns_(), line_(), eof_(false), first_(true) {}
  virtual ~FsJsonlFileReader()
  {
    if (OB_NOT_NULL(file_)) {
      fclose(file_);
      file_ = nullptr;
    }
  }
  int init(const char *path, const FileScanOptions &options)
  {
    int ret = OB_SUCCESS;
    int64_t row_count = 0;
    UNUSED(options);
    if (OB_FAIL(fs_infer_jsonl(path, columns_, row_count))) {
    } else if (OB_ISNULL(file_ = fopen(path, "rb"))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open jsonl file", K(ret), K(path), K(errno));
    }
    return ret;
  }
  virtual int next_row(bool &eof) override
  {
    int ret = OB_SUCCESS;
    eof = false;
    ObArenaAllocator arena;
    while (OB_SUCC(ret) && !eof) {
      if (OB_FAIL(fs_read_logical_line(file_, line_, eof))) {
      } else if (eof) {
        break;
      } else if (line_.empty()) {
        continue;
      } else {
        ObJsonObject *object = nullptr;
        if (OB_FAIL(fs_parse_json_line(arena, line_, object))) {
        } else {
          values_.assign(columns_.size(), std::string());
          nulls_.assign(columns_.size(), true);
          for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(columns_.size()); ++i) {
            ObString key(static_cast<int32_t>(columns_[i].name_.size()),
                         const_cast<char *>(columns_[i].name_.c_str()));
            ObJsonNode *value = object->get_value(key);
            if (OB_ISNULL(value) || value->json_type() == ObJsonNodeType::J_NULL) {
              nulls_[i] = true;
            } else {
              FsLogicalType type = ST_VARCHAR;
              std::string text;
              bool is_null = false;
              fs_classify_json_scalar(value, type, text, is_null);
              values_[i] = text;
              nulls_[i] = is_null;
            }
          }
          if (OB_SUCC(ret)) {
            ++row_count_;
            break;
          }
        }
      }
    }
    if (OB_SUCC(ret) && eof) {
      ret = OB_SUCCESS;
    }
    return ret;
  }
private:
  FILE *file_;
  std::string file_path_;
  std::vector<FsColumnState> columns_;
  std::string line_;
  bool eof_;
  bool first_;
};

////////////////////////////////////////////////////////////////////////////////
// Parquet

struct FsPqThriftReader
{
  FsPqThriftReader(const char *buf, const int64_t len)
      : buf_(reinterpret_cast<const uint8_t *>(buf)), len_(len), pos_(0), last_fid_(0) {}

  int read_byte(uint8_t &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_INVALID_DATA;
    } else {
      value = buf_[pos_++];
    }
    return ret;
  }
  int read_varint(uint64_t &value)
  {
    int ret = OB_SUCCESS;
    value = 0;
    int shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      uint8_t byte = 0;
      if (OB_FAIL(read_byte(byte))) {
      } else {
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if (0 == (byte & 0x80)) {
          done = true;
        } else {
          shift += 7;
          if (shift > 63) {
            ret = OB_INVALID_DATA;
          }
        }
      }
    }
    return ret;
  }
  int read_zigzag(int64_t &value)
  {
    int ret = OB_SUCCESS;
    uint64_t raw = 0;
    if (OB_FAIL(read_varint(raw))) {
    } else {
      value = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }
    return ret;
  }
  // returns true when a field is read, false on STOP
  int read_field(uint8_t &type, int16_t &fid, bool &stop)
  {
    int ret = OB_SUCCESS;
    uint8_t header = 0;
    stop = false;
    if (OB_FAIL(read_byte(header))) {
    } else {
      type = header & 0x0f;
      if (0 == type) {
        stop = true;
      } else {
        const uint8_t delta = header >> 4;
        if (0 == delta) {
          int64_t raw_fid = 0;
          if (OB_FAIL(read_zigzag(raw_fid))) {
          } else {
            fid = static_cast<int16_t>(raw_fid);
          }
        } else {
          fid = static_cast<int16_t>(last_fid_ + delta);
        }
        last_fid_ = fid;
      }
    }
    return ret;
  }
  int read_binary(std::string &out)
  {
    int ret = OB_SUCCESS;
    uint64_t size = 0;
    out.clear();
    if (OB_FAIL(read_varint(size))) {
    } else if (pos_ + static_cast<int64_t>(size) > len_) {
      ret = OB_INVALID_DATA;
    } else {
      out.assign(reinterpret_cast<const char *>(buf_ + pos_), size);
      pos_ += static_cast<int64_t>(size);
    }
    return ret;
  }
  int skip_list()
  {
    int ret = OB_SUCCESS;
    uint8_t header = 0;
    if (OB_FAIL(read_byte(header))) {
    } else {
      const uint8_t elem_type = header & 0x0f;
      uint64_t size = header >> 4;
      if (15 == size) {
        if (OB_FAIL(read_varint(size))) {
        }
      }
      for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
        if (OB_FAIL(skip_value(elem_type))) {
        }
      }
    }
    return ret;
  }
  int skip_value(uint8_t type)
  {
    int ret = OB_SUCCESS;
    uint8_t byte = 0;
    uint64_t varint = 0;
    std::string binary;
    switch (type) {
      case 1:
      case 2:
        break;
      case 3:
        if (OB_FAIL(read_byte(byte))) {
        }
        break;
      case 4:
      case 5:
      case 6:
        if (OB_FAIL(read_zigzag(reinterpret_cast<int64_t &>(varint)))) {
        }
        break;
      case 7: {
        if (pos_ + 8 > len_) {
          ret = OB_INVALID_DATA;
        } else {
          pos_ += 8;
        }
        break;
      }
      case 8:
        if (OB_FAIL(read_binary(binary))) {
        }
        break;
      case 9:
      case 10:
        if (OB_FAIL(skip_list())) {
        }
        break;
      case 11: {
        uint64_t size = 0;
        uint8_t header = 0;
        if (OB_FAIL(read_varint(size))) {
        } else if (0 == size) {
        } else if (OB_FAIL(read_byte(header))) {
        } else {
          const uint8_t key_type = header >> 4;
          const uint8_t value_type = header & 0x0f;
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            if (OB_FAIL(skip_value(key_type))) {
            } else if (OB_FAIL(skip_value(value_type))) {
            }
          }
        }
        break;
      }
      case 12: {
        const int16_t outer = last_fid_;
        last_fid_ = 0;
        bool stop = false;
        while (OB_SUCC(ret) && !stop) {
          uint8_t field_type = 0;
          int16_t field_id = 0;
          if (OB_FAIL(read_field(field_type, field_id, stop))) {
          } else if (!stop && OB_FAIL(skip_value(field_type))) {
          }
        }
        last_fid_ = outer;
        break;
      }
      default:
        ret = OB_INVALID_DATA;
        break;
    }
    return ret;
  }

  const uint8_t *buf_;
  int64_t len_;
  int64_t pos_;
  int16_t last_fid_;
};

struct FsPqColumnChunk
{
  FsPqColumnChunk() : type_(0), codec_(0), num_values_(0), data_page_offset_(0),
                    dict_page_offset_(-1), total_compressed_size_(0) {}
  int32_t type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dict_page_offset_;
  int64_t total_compressed_size_;
};

struct FsPqRowGroup
{
  FsPqRowGroup() : num_rows_(0) {}
  int64_t num_rows_;
  std::vector<FsPqColumnChunk> columns_;
};

struct FsPqSchemaElement
{
  FsPqSchemaElement() : type_(-1), type_length_(0), repetition_(0), converted_(-1), num_children_(0) {}
  int32_t type_;
  int32_t type_length_;
  int32_t repetition_;
  int32_t converted_;
  int32_t num_children_;
  std::string name_;
};

struct FsPqPageHeader
{
  FsPqPageHeader() : type_(-1), uncompressed_size_(0), compressed_size_(0),
                   num_values_(0), encoding_(0), def_encoding_(0), rep_encoding_(0),
                   dict_num_values_(0), dict_encoding_(0), v2_num_values_(0),
                   v2_num_nulls_(0), v2_num_rows_(0), v2_encoding_(0),
                   v2_def_len_(0), v2_rep_len_(0), v2_is_compressed_(true) {}
  int32_t type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int64_t num_values_;
  int32_t encoding_;
  int32_t def_encoding_;
  int32_t rep_encoding_;
  int64_t dict_num_values_;
  int32_t dict_encoding_;
  int64_t v2_num_values_;
  int64_t v2_num_nulls_;
  int64_t v2_num_rows_;
  int32_t v2_encoding_;
  int32_t v2_def_len_;
  int32_t v2_rep_len_;
  bool v2_is_compressed_;
};

int fs_pq_parse_schema_element(FsPqThriftReader &reader, FsPqSchemaElement &element)
{
  const int16_t outer_fid = reader.last_fid_;
  reader.last_fid_ = 0;
  int ret = OB_SUCCESS;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int16_t fid = 0;
    if (OB_FAIL(reader.read_field(type, fid, stop))) {
    } else if (stop) {
      break;
    } else if (1 == fid && 5 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        element.type_ = static_cast<int32_t>(value);
      }
    } else if (2 == fid && 5 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        element.type_length_ = static_cast<int32_t>(value);
      }
    } else if (3 == fid && 5 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        element.repetition_ = static_cast<int32_t>(value);
      }
    } else if (4 == fid && 8 == type) {
      if (OB_FAIL(reader.read_binary(element.name_))) {
      }
    } else if (5 == fid && 5 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        element.num_children_ = static_cast<int32_t>(value);
      }
    } else if (6 == fid && 5 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        element.converted_ = static_cast<int32_t>(value);
      }
    } else if (OB_FAIL(reader.skip_value(type))) {
    }
  }
  reader.last_fid_ = outer_fid;
  return ret;
}

int fs_pq_parse_column_meta(FsPqThriftReader &reader, FsPqColumnChunk &chunk)
{
  const int16_t outer_fid = reader.last_fid_;
  reader.last_fid_ = 0;
  int ret = OB_SUCCESS;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int16_t fid = 0;
    if (OB_FAIL(reader.read_field(type, fid, stop))) {
    } else if (stop) {
      break;
    } else if (1 == fid && 5 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        chunk.type_ = static_cast<int32_t>(value);
      }
    } else if (4 == fid && 5 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        chunk.codec_ = static_cast<int32_t>(value);
      }
    } else if (5 == fid && 6 == type) {
      if (OB_FAIL(reader.read_zigzag(chunk.num_values_))) {
      }
    } else if (7 == fid && 6 == type) {
      if (OB_FAIL(reader.read_zigzag(chunk.total_compressed_size_))) {
      }
    } else if (9 == fid && 6 == type) {
      if (OB_FAIL(reader.read_zigzag(chunk.data_page_offset_))) {
      }
    } else if (11 == fid && 6 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        chunk.dict_page_offset_ = value;
      }
    } else if (OB_FAIL(reader.skip_value(type))) {
    }
  }
  reader.last_fid_ = outer_fid;
  return ret;
}

int fs_pq_parse_column_chunk(FsPqThriftReader &reader, FsPqColumnChunk &chunk)
{
  const int16_t outer_fid = reader.last_fid_;
  reader.last_fid_ = 0;
  int ret = OB_SUCCESS;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int16_t fid = 0;
    if (OB_FAIL(reader.read_field(type, fid, stop))) {
    } else if (stop) {
      break;
    } else if (3 == fid && 12 == type) {
      if (OB_FAIL(fs_pq_parse_column_meta(reader, chunk))) {
      }
    } else if (OB_FAIL(reader.skip_value(type))) {
    }
  }
  reader.last_fid_ = outer_fid;
  return ret;
}

int fs_pq_parse_row_group(FsPqThriftReader &reader, FsPqRowGroup &group)
{
  const int16_t outer_fid = reader.last_fid_;
  reader.last_fid_ = 0;
  int ret = OB_SUCCESS;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int16_t fid = 0;
    if (OB_FAIL(reader.read_field(type, fid, stop))) {
    } else if (stop) {
      break;
    } else if (1 == fid && 9 == type) {
      uint8_t header = 0;
      if (OB_FAIL(reader.read_byte(header))) {
      } else {
        uint64_t size = header >> 4;
        const uint8_t elem_type = header & 0x0f;
        if (15 == size && OB_FAIL(reader.read_varint(size))) {
        } else if (elem_type != 12) {
          ret = OB_INVALID_DATA;
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            FsPqColumnChunk chunk;
            if (OB_FAIL(fs_pq_parse_column_chunk(reader, chunk))) {
            } else {
              group.columns_.push_back(chunk);
            }
          }
        }
      }
    } else if (3 == fid && 6 == type) {
      if (OB_FAIL(reader.read_zigzag(group.num_rows_))) {
      }
    } else if (OB_FAIL(reader.skip_value(type))) {
    }
  }
  reader.last_fid_ = outer_fid;
  return ret;
}

struct FsPqFileMeta
{
  FsPqFileMeta() : num_rows_(0) {}
  int64_t num_rows_;
  std::vector<FsPqSchemaElement> schema_;
  std::vector<FsPqRowGroup> row_groups_;
};

int fs_pq_parse_file_meta(const char *buf, const int64_t len, FsPqFileMeta &meta)
{
  int ret = OB_SUCCESS;
  FsPqThriftReader reader(buf, len);
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int16_t fid = 0;
    if (OB_FAIL(reader.read_field(type, fid, stop))) {
    } else if (stop) {
      break;
    } else if (3 == fid && 6 == type) {
      if (OB_FAIL(reader.read_zigzag(meta.num_rows_))) {
      }
    } else if (2 == fid && 9 == type) {
      uint8_t header = 0;
      if (OB_FAIL(reader.read_byte(header))) {
      } else {
        uint64_t size = header >> 4;
        const uint8_t elem_type = header & 0x0f;
        if (15 == size && OB_FAIL(reader.read_varint(size))) {
        } else if (elem_type != 12) {
          ret = OB_INVALID_DATA;
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            FsPqSchemaElement element;
            if (OB_FAIL(fs_pq_parse_schema_element(reader, element))) {
            } else {
              meta.schema_.push_back(element);
            }
          }
        }
      }
    } else if (4 == fid && 9 == type) {
      uint8_t header = 0;
      if (OB_FAIL(reader.read_byte(header))) {
      } else {
        uint64_t size = header >> 4;
        const uint8_t elem_type = header & 0x0f;
        if (15 == size && OB_FAIL(reader.read_varint(size))) {
        } else if (elem_type != 12) {
          ret = OB_INVALID_DATA;
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            FsPqRowGroup group;
            if (OB_FAIL(fs_pq_parse_row_group(reader, group))) {
            } else {
              meta.row_groups_.push_back(group);
            }
          }
        }
      }
    } else if (OB_FAIL(reader.skip_value(type))) {
    }
  }
  return ret;
}

int fs_pq_parse_page_header(const char *buf, const int64_t len, FsPqPageHeader &header, int64_t &consumed)
{
  int ret = OB_SUCCESS;
  FsPqThriftReader reader(buf, len);
  bool stop = false;
  consumed = 0;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int16_t fid = 0;
    if (OB_FAIL(reader.read_field(type, fid, stop))) {
    } else if (stop) {
      break;
    } else if (1 == fid && 5 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        header.type_ = static_cast<int32_t>(value);
      }
    } else if (2 == fid && 5 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        header.uncompressed_size_ = static_cast<int32_t>(value);
      }
    } else if (3 == fid && 5 == type) {
      int64_t value = 0;
      if (OB_FAIL(reader.read_zigzag(value))) {
      } else {
        header.compressed_size_ = static_cast<int32_t>(value);
      }
    } else if (5 == fid && 12 == type) {
      const int16_t outer_fid = reader.last_fid_;
      reader.last_fid_ = 0;
      bool sub_stop = false;
      while (OB_SUCC(ret) && !sub_stop) {
        uint8_t sub_type = 0;
        int16_t sub_fid = 0;
        if (OB_FAIL(reader.read_field(sub_type, sub_fid, sub_stop))) {
        } else if (sub_stop) {
          break;
        } else if (1 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.num_values_ = value;
          }
        } else if (2 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.encoding_ = static_cast<int32_t>(value);
          }
        } else if (3 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.def_encoding_ = static_cast<int32_t>(value);
          }
        } else if (4 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.rep_encoding_ = static_cast<int32_t>(value);
          }
        } else if (OB_FAIL(reader.skip_value(sub_type))) {
        }
      }
      reader.last_fid_ = outer_fid;
    } else if (7 == fid && 12 == type) {
      const int16_t outer_fid = reader.last_fid_;
      reader.last_fid_ = 0;
      bool sub_stop = false;
      while (OB_SUCC(ret) && !sub_stop) {
        uint8_t sub_type = 0;
        int16_t sub_fid = 0;
        if (OB_FAIL(reader.read_field(sub_type, sub_fid, sub_stop))) {
        } else if (sub_stop) {
          break;
        } else if (1 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.dict_num_values_ = value;
          }
        } else if (2 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.dict_encoding_ = static_cast<int32_t>(value);
          }
        } else if (OB_FAIL(reader.skip_value(sub_type))) {
        }
      }
      reader.last_fid_ = outer_fid;
    } else if (8 == fid && 12 == type) {
      const int16_t outer_fid = reader.last_fid_;
      reader.last_fid_ = 0;
      bool sub_stop = false;
      while (OB_SUCC(ret) && !sub_stop) {
        uint8_t sub_type = 0;
        int16_t sub_fid = 0;
        if (OB_FAIL(reader.read_field(sub_type, sub_fid, sub_stop))) {
        } else if (sub_stop) {
          break;
        } else if (1 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.v2_num_values_ = value;
          }
        } else if (2 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.v2_num_nulls_ = value;
          }
        } else if (3 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.v2_num_rows_ = value;
          }
        } else if (4 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.v2_encoding_ = static_cast<int32_t>(value);
          }
        } else if (5 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.v2_def_len_ = static_cast<int32_t>(value);
          }
        } else if (6 == sub_fid && 5 == sub_type) {
          int64_t value = 0;
          if (OB_FAIL(reader.read_zigzag(value))) {
          } else {
            header.v2_rep_len_ = static_cast<int32_t>(value);
          }
        } else if (7 == sub_fid && (1 == sub_type || 2 == sub_type)) {
          header.v2_is_compressed_ = (1 == sub_type);
        } else if (OB_FAIL(reader.skip_value(sub_type))) {
        }
      }
      reader.last_fid_ = outer_fid;
    } else if (OB_FAIL(reader.skip_value(type))) {
    }
  }
  if (OB_SUCC(ret)) {
    consumed = reader.pos_;
  }
  return ret;
}

int fs_pq_snappy_decompress(const char *input, const int64_t input_len,
                         std::string &output)
{
  int ret = OB_SUCCESS;
  output.clear();
  int64_t pos = 0;
  uint64_t expected_size = 0;
  // varint uncompressed length
  int shift = 0;
  while (OB_SUCC(ret)) {
    if (pos >= input_len) {
      ret = OB_INVALID_DATA;
    } else {
      const uint8_t byte = static_cast<uint8_t>(input[pos++]);
      expected_size |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if (0 == (byte & 0x80)) {
        break;
      }
      shift += 7;
      if (shift > 63) {
        ret = OB_INVALID_DATA;
      }
    }
  }
  if (OB_SUCC(ret)) {
    output.reserve(expected_size);
    while (OB_SUCC(ret) && pos < input_len && output.size() < expected_size) {
      const uint8_t tag = static_cast<uint8_t>(input[pos++]);
      const int kind = tag & 0x03;
      if (0 == kind) {
        int64_t len = tag >> 2;
        if (len < 60) {
          ++len;
        } else {
          const int extra = static_cast<int>(len) - 59;
          if (pos + extra > input_len) {
            ret = OB_INVALID_DATA;
          } else {
            uint64_t value = 0;
            for (int i = 0; i < extra; ++i) {
              value |= static_cast<uint64_t>(static_cast<uint8_t>(input[pos + i])) << (8 * i);
            }
            pos += extra;
            len = static_cast<int64_t>(value) + 1;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (pos + len > input_len) {
          ret = OB_INVALID_DATA;
        } else {
          output.append(input + pos, len);
          pos += len;
        }
      } else {
        int64_t len = 0;
        int64_t offset = 0;
        if (1 == kind) {
          if (pos >= input_len) {
            ret = OB_INVALID_DATA;
          } else {
            len = ((tag >> 2) & 0x07) + 4;
            offset = ((static_cast<int64_t>(tag) & 0xe0) << 3)
                     | static_cast<uint8_t>(input[pos++]);
          }
        } else if (2 == kind) {
          if (pos + 2 > input_len) {
            ret = OB_INVALID_DATA;
          } else {
            len = (tag >> 2) + 1;
            offset = static_cast<uint8_t>(input[pos])
                     | (static_cast<int64_t>(static_cast<uint8_t>(input[pos + 1])) << 8);
            pos += 2;
          }
        } else {
          if (pos + 4 > input_len) {
            ret = OB_INVALID_DATA;
          } else {
            len = (tag >> 2) + 1;
            offset = static_cast<uint8_t>(input[pos])
                     | (static_cast<int64_t>(static_cast<uint8_t>(input[pos + 1])) << 8)
                     | (static_cast<int64_t>(static_cast<uint8_t>(input[pos + 2])) << 16)
                     | (static_cast<int64_t>(static_cast<uint8_t>(input[pos + 3])) << 24);
            pos += 4;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (offset <= 0 || offset > static_cast<int64_t>(output.size())) {
          ret = OB_INVALID_DATA;
        } else {
          const size_t start = output.size() - offset;
          for (int64_t i = 0; i < len; ++i) {
            output.push_back(output[start + i]);
          }
        }
      }
    }
    if (OB_SUCC(ret) && output.size() != expected_size) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

int fs_pq_read_varint(const uint8_t *buf, const int64_t len, int64_t &pos, uint64_t &value)
{
  int ret = OB_SUCCESS;
  value = 0;
  int shift = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (pos >= len) {
      ret = OB_INVALID_DATA;
    } else {
      const uint8_t byte = buf[pos++];
      value |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if (0 == (byte & 0x80)) {
        done = true;
      } else {
        shift += 7;
        if (shift > 63) {
          ret = OB_INVALID_DATA;
        }
      }
    }
  }
  return ret;
}

int fs_pq_decode_hybrid(const uint8_t *buf, const int64_t len, const int bit_width,
                     const int64_t value_count, std::vector<uint32_t> &out)
{
  int ret = OB_SUCCESS;
  out.clear();
  out.reserve(value_count);
  int64_t pos = 0;
  while (OB_SUCC(ret) && static_cast<int64_t>(out.size()) < value_count) {
    uint64_t header = 0;
    if (OB_FAIL(fs_pq_read_varint(buf, len, pos, header))) {
    } else if (0 == (header & 1)) {
      const int64_t run = static_cast<int64_t>(header >> 1);
      const int byte_width = (bit_width + 7) / 8;
      uint64_t value = 0;
      if (pos + byte_width > len) {
        ret = OB_INVALID_DATA;
      } else {
        for (int i = 0; i < byte_width; ++i) {
          value |= static_cast<uint64_t>(buf[pos + i]) << (8 * i);
        }
        pos += byte_width;
        for (int64_t i = 0; i < run && static_cast<int64_t>(out.size()) < value_count; ++i) {
          out.push_back(static_cast<uint32_t>(value));
        }
      }
    } else {
      const int64_t groups = static_cast<int64_t>(header >> 1);
      const int64_t total_bits = groups * 8 * bit_width;
      const int64_t total_bytes = (total_bits + 7) / 8;
      if (pos + total_bytes > len) {
        ret = OB_INVALID_DATA;
      } else {
        const uint64_t mask = (bit_width >= 32) ? 0xffffffffffffffffULL
                                                : ((1ULL << bit_width) - 1);
        for (int64_t i = 0; i < groups * 8 && static_cast<int64_t>(out.size()) < value_count; ++i) {
          uint64_t value = 0;
          const int64_t bit_offset = i * bit_width;
          for (int b = 0; b < bit_width; ++b) {
            const int64_t abs_bit = bit_offset + b;
            const uint8_t byte = buf[pos + abs_bit / 8];
            if (byte & (1u << (abs_bit % 8))) {
              value |= 1ULL << b;
            }
          }
          out.push_back(static_cast<uint32_t>(value & mask));
        }
        pos += total_bytes;
      }
    }
  }
  if (OB_SUCC(ret) && static_cast<int64_t>(out.size()) < value_count) {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

void fs_pq_days_to_date(const int32_t days, std::string &out)
{
  // civil_from_days (Howard Hinnant's algorithm)
  int64_t z = static_cast<int64_t>(days) + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t y = static_cast<uint32_t>(yoe) + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  const uint32_t year = y + (m <= 2);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04u-%02u-%02u", year, m, d);
  out.assign(buf);
}

void fs_pq_micros_to_datetime(const int64_t micros, std::string &out)
{
  int64_t seconds = micros / 1000000;
  int64_t frac = micros % 1000000;
  if (frac < 0) {
    frac += 1000000;
    --seconds;
  }
  int64_t days = seconds / 86400;
  int64_t rem = seconds % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  std::string date;
  fs_pq_days_to_date(static_cast<int32_t>(days), date);
  char buf[48];
  snprintf(buf, sizeof(buf), "%s %02lld:%02lld:%02lld.%06lld", date.c_str(),
           static_cast<long long>(rem / 3600),
           static_cast<long long>((rem % 3600) / 60),
           static_cast<long long>(rem % 60),
           static_cast<long long>(frac));
  out.assign(buf);
}

int fs_pq_plain_value(const uint8_t *buf, const int64_t len, int64_t &pos,
                   const FsPqSchemaElement &element, std::string &out)
{
  int ret = OB_SUCCESS;
  char tmp[64];
  switch (element.type_) {
    case 0: { // BOOLEAN handled by caller
      ret = OB_ERR_UNEXPECTED;
      break;
    }
    case 1: { // INT32
      if (pos + 4 > len) {
        ret = OB_INVALID_DATA;
      } else {
        int32_t value = 0;
        memcpy(&value, buf + pos, 4);
        pos += 4;
        if (6 == element.converted_) { // DATE
          fs_pq_days_to_date(value, out);
        } else {
          snprintf(tmp, sizeof(tmp), "%d", value);
          out.assign(tmp);
        }
      }
      break;
    }
    case 2: { // INT64
      if (pos + 8 > len) {
        ret = OB_INVALID_DATA;
      } else {
        int64_t value = 0;
        memcpy(&value, buf + pos, 8);
        pos += 8;
        if (9 == element.converted_) { // TIMESTAMP_MILLIS
          fs_pq_micros_to_datetime(value * 1000, out);
        } else if (10 == element.converted_) { // TIMESTAMP_MICROS
          fs_pq_micros_to_datetime(value, out);
        } else {
          snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(value));
          out.assign(tmp);
        }
      }
      break;
    }
    case 4: { // FLOAT
      if (pos + 4 > len) {
        ret = OB_INVALID_DATA;
      } else {
        float value = 0;
        memcpy(&value, buf + pos, 4);
        pos += 4;
        snprintf(tmp, sizeof(tmp), "%.9g", static_cast<double>(value));
        out.assign(tmp);
      }
      break;
    }
    case 5: { // DOUBLE
      if (pos + 8 > len) {
        ret = OB_INVALID_DATA;
      } else {
        double value = 0;
        memcpy(&value, buf + pos, 8);
        pos += 8;
        snprintf(tmp, sizeof(tmp), "%.17g", value);
        out.assign(tmp);
      }
      break;
    }
    case 6: { // BYTE_ARRAY
      if (pos + 4 > len) {
        ret = OB_INVALID_DATA;
      } else {
        int32_t size = 0;
        memcpy(&size, buf + pos, 4);
        pos += 4;
        if (size < 0 || pos + size > len) {
          ret = OB_INVALID_DATA;
        } else {
          out.assign(reinterpret_cast<const char *>(buf + pos), size);
          pos += size;
        }
      }
      break;
    }
    case 7: { // FIXED_LEN_BYTE_ARRAY
      if (element.type_length_ < 0 || pos + element.type_length_ > len) {
        ret = OB_INVALID_DATA;
      } else {
        out.assign(reinterpret_cast<const char *>(buf + pos), element.type_length_);
        pos += element.type_length_;
      }
      break;
    }
    default:
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported parquet physical type", K(ret), K(element.type_));
      break;
  }
  return ret;
}

struct FsPqColumnValues
{
  FsPqColumnValues() : values_(), defined_() {}
  std::vector<std::string> values_;
  std::vector<bool> defined_;
};

int fs_pq_decode_dictionary(const uint8_t *buf, const int64_t len,
                         const FsPqSchemaElement &element, const int64_t count,
                         std::vector<std::string> &dict)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  dict.clear();
  dict.reserve(count);
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    std::string value;
    if (OB_FAIL(fs_pq_plain_value(buf, len, pos, element, value))) {
    } else {
      dict.push_back(value);
    }
  }
  return ret;
}

int fs_pq_decode_values(const uint8_t *buf, const int64_t len, int64_t &pos,
                     const FsPqSchemaElement &element, const int32_t encoding,
                     const int64_t value_count,
                     const std::vector<std::string> &dict,
                     std::vector<std::string> &values)
{
  int ret = OB_SUCCESS;
  values.clear();
  values.reserve(value_count);
  if (0 == encoding) { // PLAIN
    if (0 == element.type_) { // BOOLEAN bit packed
      for (int64_t i = 0; OB_SUCC(ret) && i < value_count; ++i) {
        const int64_t byte_index = pos + i / 8;
        if (byte_index >= len) {
          ret = OB_INVALID_DATA;
        } else {
          const bool value = (buf[byte_index] >> (i % 8)) & 1;
          values.push_back(value ? "true" : "false");
        }
      }
      pos += (value_count + 7) / 8;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < value_count; ++i) {
        std::string value;
        if (OB_FAIL(fs_pq_plain_value(buf, len, pos, element, value))) {
        } else {
          values.push_back(value);
        }
      }
    }
  } else if (2 == encoding || 8 == encoding) { // PLAIN_DICTIONARY / RLE_DICTIONARY
    if (pos >= len) {
      ret = OB_INVALID_DATA;
    } else {
      const int bit_width = buf[pos++];
      std::vector<uint32_t> indexes;
      if (OB_FAIL(fs_pq_decode_hybrid(buf + pos, len - pos, bit_width, value_count, indexes))) {
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < value_count; ++i) {
          if (indexes[i] >= dict.size()) {
            ret = OB_INVALID_DATA;
          } else {
            values.push_back(dict[indexes[i]]);
          }
        }
      }
    }
  } else if (3 == encoding) { // RLE (booleans only)
    if (0 == element.type_) {
      uint32_t length = 0;
      if (pos + 4 > len) {
        ret = OB_INVALID_DATA;
      } else {
        memcpy(&length, buf + pos, 4);
        pos += 4;
        std::vector<uint32_t> decoded;
        if (OB_FAIL(fs_pq_decode_hybrid(buf + pos, len - pos, 1, value_count, decoded))) {
        } else {
          for (int64_t i = 0; i < value_count; ++i) {
            values.push_back(decoded[i] ? "true" : "false");
          }
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported parquet RLE encoding for non boolean column", K(ret));
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("unsupported parquet value encoding", K(ret), K(encoding));
  }
  return ret;
}

int fs_pq_read_page_header(FILE *file, FsPqPageHeader &header, int64_t &header_len)
{
  int ret = OB_SUCCESS;
  const int64_t initial = 16384;
  const int64_t offset = ftell(file);
  if (OB_UNLIKELY(offset < 0)) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(fseek(file, 0, SEEK_END))) {
  } else {
    const int64_t end = ftell(file);
    if (OB_FAIL(fseek(file, offset, SEEK_SET))) {
    } else if (end <= offset) {
      ret = OB_INVALID_DATA;
      LOG_WARN("unexpected end of parquet file", K(ret), K(offset), K(end));
    } else {
      const int64_t to_read = std::min(initial, end - offset);
      std::vector<char> buffer;
      buffer.resize(to_read);
      const int64_t read_len = static_cast<int64_t>(fread(buffer.data(), 1, to_read, file));
      if (read_len != to_read) {
        ret = OB_INVALID_DATA;
        LOG_WARN("failed to read parquet page header", K(ret), K(offset), K(read_len), K(to_read));
      } else {
        int64_t consumed = 0;
        if (OB_FAIL(fs_pq_parse_page_header(buffer.data(), read_len, header, consumed))) {
        } else if (consumed <= 0) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(fseek(file, offset + consumed, SEEK_SET))) {
        } else {
          header_len = consumed;
        }
      }
    }
  }
  return ret;
}

int fs_pq_read_column_chunk(FILE *file, const FsPqSchemaElement &element,
                         const FsPqColumnChunk &chunk, FsPqColumnValues &out)
{
  int ret = OB_SUCCESS;
  std::vector<std::string> dict;
  int64_t total = 0;
  if (0 != element.repetition_ && 1 != element.repetition_) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("repeated parquet column is not supported", K(ret), K(element.repetition_));
  } else {
    if (chunk.dict_page_offset_ > 0) {
      if (OB_FAIL(fseek(file, chunk.dict_page_offset_, SEEK_SET))) {
      } else {
        FsPqPageHeader header;
        int64_t header_len = 0;
        if (OB_FAIL(fs_pq_read_page_header(file, header, header_len))) {
        } else if (2 != header.type_) {
          ret = OB_INVALID_DATA;
        } else {
          std::vector<char> compressed;
          compressed.resize(header.compressed_size_);
          if (header.compressed_size_ > 0
              && 1 != fread(compressed.data(), header.compressed_size_, 1, file)) {
            ret = OB_INVALID_DATA;
          } else {
            const char *page_data = compressed.data();
            int64_t page_len = header.compressed_size_;
            std::string decompressed;
            if (1 == chunk.codec_) { // SNAPPY
              if (OB_FAIL(fs_pq_snappy_decompress(page_data, page_len, decompressed))) {
              } else {
                page_data = decompressed.data();
                page_len = static_cast<int64_t>(decompressed.size());
              }
            } else if (0 != chunk.codec_) {
              ret = OB_NOT_SUPPORTED;
              LOG_WARN("unsupported parquet compression codec", K(ret), K(chunk.codec_));
            }
            if (OB_SUCC(ret) && OB_FAIL(fs_pq_decode_dictionary(reinterpret_cast<const uint8_t *>(page_data), page_len, element,
                                                             header.dict_num_values_, dict))) {
            }
          }
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(fseek(file, chunk.data_page_offset_, SEEK_SET))) {
    }
    while (OB_SUCC(ret) && total < chunk.num_values_) {
      FsPqPageHeader header;
      int64_t header_len = 0;
      if (OB_FAIL(fs_pq_read_page_header(file, header, header_len))) {
      } else if (0 == header.type_ || 3 == header.type_) {
        const bool is_v2 = (3 == header.type_);
        const int64_t page_size = header.compressed_size_;
        std::vector<char> raw;
        raw.resize(page_size);
        if (page_size > 0 && 1 != fread(raw.data(), page_size, 1, file)) {
          ret = OB_INVALID_DATA;
        } else {
          const int64_t num_values = is_v2 ? header.v2_num_values_ : header.num_values_;
          const int32_t encoding = is_v2 ? header.v2_encoding_ : header.encoding_;
          int64_t rep_len = 0;
          int64_t def_len = 0;
          if (is_v2) {
            rep_len = header.v2_rep_len_;
            def_len = header.v2_def_len_;
          }
          std::string decompressed;
          std::string data_buf;
          const char *page_data = raw.data();
          int64_t page_len = page_size;
          int64_t value_pos = 0;
          if (is_v2) {
            if (rep_len + def_len > page_size) {
              ret = OB_INVALID_DATA;
            } else {
              const int64_t data_len = page_size - rep_len - def_len;
              const char *compressed_values = raw.data() + rep_len + def_len;
              if (header.v2_is_compressed_ && 1 == chunk.codec_) {
                if (OB_FAIL(fs_pq_snappy_decompress(compressed_values, data_len, decompressed))) {
                } else {
                  data_buf = decompressed;
                }
              } else if (header.v2_is_compressed_ && 0 != chunk.codec_) {
                ret = OB_NOT_SUPPORTED;
                LOG_WARN("unsupported parquet compression codec", K(ret), K(chunk.codec_));
              } else {
                data_buf.assign(compressed_values, data_len);
              }
              if (OB_SUCC(ret)) {
                page_data = data_buf.data();
                page_len = static_cast<int64_t>(data_buf.size());
                value_pos = 0;
              }
            }
          } else {
            if (1 == chunk.codec_) {
              if (OB_FAIL(fs_pq_snappy_decompress(page_data, page_len, decompressed))) {
              } else {
                page_data = decompressed.data();
                page_len = static_cast<int64_t>(decompressed.size());
              }
            } else if (0 != chunk.codec_) {
              ret = OB_NOT_SUPPORTED;
              LOG_WARN("unsupported parquet compression codec", K(ret), K(chunk.codec_));
            }
          }
          if (OB_SUCC(ret)) {
            const int64_t max_def = (1 == element.repetition_) ? 1 : 0;
            std::vector<uint32_t> def_levels;
            int64_t pos = 0;
            if (is_v2) {
              if (max_def > 0) {
                if (OB_FAIL(fs_pq_decode_hybrid(reinterpret_cast<const uint8_t *>(raw.data()) + rep_len, def_len, 1,
                                             num_values, def_levels))) {
                }
              } else {
                def_levels.assign(num_values, 1);
              }
            } else if (max_def > 0) {
              uint32_t def_size = 0;
              if (page_len < 4) {
                ret = OB_INVALID_DATA;
              } else {
                memcpy(&def_size, page_data, 4);
                pos = 4;
                if (pos + def_size > page_len) {
                  ret = OB_INVALID_DATA;
                } else if (OB_FAIL(fs_pq_decode_hybrid(
                             reinterpret_cast<const uint8_t *>(page_data) + pos, def_size,
                             1, num_values, def_levels))) {
                } else {
                  pos += def_size;
                }
              }
            } else {
              def_levels.assign(num_values, 1);
            }
            if (OB_SUCC(ret)) {
              int64_t defined_count = 0;
              for (int64_t i = 0; i < num_values; ++i) {
                defined_count += (def_levels[i] >= static_cast<uint32_t>(max_def)) ? 1 : 0;
              }
              std::vector<std::string> values;
              if (OB_FAIL(fs_pq_decode_values(
                      reinterpret_cast<const uint8_t *>(page_data),
                      is_v2 ? page_len : page_len, pos, element, encoding, defined_count,
                      dict, values))) {
              } else {
                int64_t value_idx = 0;
                for (int64_t i = 0; i < num_values; ++i) {
                  if (def_levels[i] >= static_cast<uint32_t>(max_def)) {
                    out.values_.push_back(values[value_idx++]);
                    out.defined_.push_back(true);
                  } else {
                    out.values_.push_back(std::string());
                    out.defined_.push_back(false);
                  }
                }
                total += num_values;
              }
            }
          }
        }
      } else {
        // skip index pages
        if (OB_FAIL(fseek(file, header.compressed_size_, SEEK_CUR))) {
        }
      }
    }
  }
  return ret;
}

class FsParquetFileReader : public FileScanReader
{
public:
  FsParquetFileReader() : file_(nullptr), meta_(), group_idx_(-1), row_idx_(0), row_count_(0),
                        rowgroup_row_count_(0) {}
  virtual ~FsParquetFileReader()
  {
    if (OB_NOT_NULL(file_)) {
      fclose(file_);
      file_ = nullptr;
    }
  }
  int init(const char *path, const FileScanOptions &options,
           const std::vector<int64_t> &projection)
  {
    int ret = OB_SUCCESS;
    UNUSED(options);
    if (OB_ISNULL(path) || OB_ISNULL(file_ = fopen(path, "rb"))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open parquet file", K(ret), K(path), K(errno));
    } else if (OB_FAIL(load_footer())) {
    } else {
      projection_ = projection;
      if (projection_.empty()) {
        for (int64_t i = 1; i < static_cast<int64_t>(meta_.schema_.size()); ++i) {
          if (0 == meta_.schema_[i].num_children_) {
            projection_.push_back(i - 1);
          }
        }
      }
    }
    return ret;
  }
  virtual int next_row(bool &eof) override
  {
    int ret = OB_SUCCESS;
    eof = false;
    while (OB_SUCC(ret)) {
      if (row_idx_ >= rowgroup_row_count_) {
        group_idx_++;
        row_idx_ = 0;
        if (group_idx_ >= static_cast<int64_t>(meta_.row_groups_.size())) {
          eof = true;
          break;
        } else if (OB_FAIL(load_row_group(group_idx_))) {
        }
        continue;
      }
      const int64_t row = row_idx_++;
      values_.assign(projection_.size(), std::string());
      nulls_.assign(projection_.size(), true);
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(projection_.size()); ++i) {
        const int64_t col = projection_[i];
        if (col < 0 || col >= static_cast<int64_t>(column_values_.size())) {
          ret = OB_INVALID_DATA;
        } else if (row < static_cast<int64_t>(column_values_[col].values_.size())) {
          values_[i] = column_values_[col].values_[row];
          nulls_[i] = !column_values_[col].defined_[row];
        } else {
          ret = OB_INVALID_DATA;
        }
      }
      if (OB_SUCC(ret)) {
        ++row_count_;
        break;
      }
    }
    return ret;
  }
private:
  int load_footer()
  {
    int ret = OB_SUCCESS;
    int64_t file_size = 0;
    if (OB_FAIL(fseek(file_, 0, SEEK_END))) {
    } else if ((file_size = ftell(file_)) < 8) {
      ret = OB_INVALID_DATA;
    } else {
      char head[4] = {0};
      char tail[8] = {0};
      if (OB_FAIL(fseek(file_, 0, SEEK_SET)) || 1 != fread(head, 4, 1, file_)) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(fseek(file_, file_size - 8, SEEK_SET))
                 || 1 != fread(tail, 8, 1, file_)) {
        ret = OB_INVALID_DATA;
      } else if (0 != memcmp(head, "PAR1", 4) || 0 != memcmp(tail + 4, "PAR1", 4)) {
        ret = OB_INVALID_DATA;
      } else {
        uint32_t footer_len = 0;
        memcpy(&footer_len, tail, 4);
        if (footer_len > static_cast<uint32_t>(file_size - 8)) {
          ret = OB_INVALID_DATA;
        } else {
          std::vector<char> footer;
          footer.resize(footer_len);
          if (OB_FAIL(fseek(file_, file_size - 8 - footer_len, SEEK_SET))
              || (footer_len > 0 && 1 != fread(footer.data(), footer_len, 1, file_))) {
            ret = OB_INVALID_DATA;
          } else if (OB_FAIL(fs_pq_parse_file_meta(footer.data(), footer_len, meta_))) {
          } else {
            if (meta_.schema_.empty()) {
              ret = OB_INVALID_DATA;
            }
          }
        }
      }
    }
    return ret;
  }
  int load_row_group(const int64_t index)
  {
    int ret = OB_SUCCESS;
    column_values_.clear();
    if (index < 0 || index >= static_cast<int64_t>(meta_.row_groups_.size())) {
      ret = OB_INVALID_DATA;
    } else {
      const FsPqRowGroup &group = meta_.row_groups_[index];
      rowgroup_row_count_ = group.num_rows_;
      column_values_.resize(meta_.schema_.size() - 1);
      // schema_[0] is the root element
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(projection_.size()); ++i) {
        const int64_t col = projection_[i];
        const int64_t schema_idx = col + 1;
        if (schema_idx >= static_cast<int64_t>(meta_.schema_.size())) {
          ret = OB_INVALID_DATA;
        } else if (col >= static_cast<int64_t>(group.columns_.size())) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(fs_pq_read_column_chunk(file_, meta_.schema_[schema_idx],
                                                group.columns_[col], column_values_[col]))) {
        }
      }
    }
    return ret;
  }

  FILE *file_;
  FsPqFileMeta meta_;
  std::vector<int64_t> projection_;
  std::vector<FsPqColumnValues> column_values_;
  int64_t group_idx_;
  int64_t row_idx_;
  int64_t row_count_;
  int64_t rowgroup_row_count_;
};

int fs_infer_parquet(const char *path, std::vector<FsColumnState> &columns)
{
  int ret = OB_SUCCESS;
  FILE *file = nullptr;
  if (OB_ISNULL(path) || OB_ISNULL(file = fopen(path, "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("failed to open parquet file", K(ret), K(path), K(errno));
  } else {
    FsParquetFileReader reader;
    // reuse footer parsing via a temporary reader would reopen the file; parse directly here.
    int64_t file_size = 0;
    if (OB_FAIL(fseek(file, 0, SEEK_END))) {
    } else if ((file_size = ftell(file)) < 8) {
      ret = OB_INVALID_DATA;
    } else {
      char tail[8] = {0};
      if (OB_FAIL(fseek(file, file_size - 8, SEEK_SET)) || 1 != fread(tail, 8, 1, file)) {
        ret = OB_INVALID_DATA;
      } else {
        uint32_t footer_len = 0;
        memcpy(&footer_len, tail, 4);
        std::vector<char> footer;
        footer.resize(footer_len);
        if (footer_len > static_cast<uint32_t>(file_size - 8)) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(fseek(file, file_size - 8 - footer_len, SEEK_SET))
                   || (footer_len > 0 && 1 != fread(footer.data(), footer_len, 1, file))) {
          ret = OB_INVALID_DATA;
        } else {
          FsPqFileMeta meta;
          if (OB_FAIL(fs_pq_parse_file_meta(footer.data(), footer_len, meta))) {
          } else if (meta.schema_.size() <= 1) {
            ret = OB_INVALID_DATA;
          } else if (meta.schema_[0].num_children_ != static_cast<int32_t>(meta.schema_.size() - 1)) {
            // flat schema only
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested parquet schema");
          } else {
            for (int64_t i = 1; OB_SUCC(ret) && i < static_cast<int64_t>(meta.schema_.size()); ++i) {
              const FsPqSchemaElement &element = meta.schema_[i];
              FsColumnState column;
              column.name_ = element.name_;
              column.nullable_ = (1 == element.repetition_);
              if (0 == element.type_) {
                column.type_ = ST_BOOL;
              } else if (6 == element.converted_) {
                column.type_ = ST_DATE;
              } else if (9 == element.converted_ || 10 == element.converted_) {
                column.type_ = ST_DATETIME;
              } else if (1 == element.type_ || 2 == element.type_) {
                column.type_ = ST_INT;
              } else if (4 == element.type_ || 5 == element.type_) {
                column.type_ = ST_DOUBLE;
              } else if (6 == element.type_ || 7 == element.type_) {
                column.type_ = ST_VARCHAR;
              } else {
                ret = OB_NOT_SUPPORTED;
                LOG_WARN("unsupported parquet schema type", K(ret), K(element.type_));
              }
              if (OB_SUCC(ret)) {
                columns.push_back(column);
              }
            }
          }
        }
      }
    }
    if (OB_NOT_NULL(file)) {
      fclose(file);
      file = nullptr;
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// glob

bool fs_glob_match(const char *pattern, const char *text)
{
  if ('\0' == *pattern) {
    return '\0' == *text;
  } else if ('*' == *pattern) {
    while ('*' == *pattern) {
      ++pattern;
    }
    if ('\0' == *pattern) {
      return true;
    }
    for (const char *p = text; ; ++p) {
      if (fs_glob_match(pattern, p)) {
        return true;
      } else if ('\0' == *p) {
        return false;
      }
    }
  } else if ('?' == *pattern) {
    return '\0' != *text && fs_glob_match(pattern + 1, text + 1);
  } else if ('[' == *pattern) {
    const char *p = pattern + 1;
    bool negate = false;
    if ('!' == *p || '^' == *p) {
      negate = true;
      ++p;
    }
    bool matched = false;
    bool closed = false;
    char prev = 0;
    while (*p) {
      if (']' == *p && p != pattern + 1) {
        closed = true;
        ++p;
        break;
      } else if ('-' == *p && prev != 0 && p[1] && ']' != p[1]) {
        if (*text >= prev && *text <= p[1]) {
          matched = true;
        }
        p += 2;
        prev = 0;
      } else {
        if (*text == *p) {
          matched = true;
        }
        prev = *p;
        ++p;
      }
    }
    if (!closed || '\0' == *text) {
      return false;
    }
    return (negate ? !matched : matched) && fs_glob_match(p, text + 1);
  } else {
    return *pattern == *text && fs_glob_match(pattern + 1, text + 1);
  }
}

int fs_glob_parent_dir(const char *pattern, std::string &dir, std::string &basename)
{
  int ret = OB_SUCCESS;
  const std::string full(pattern);
  const size_t slash = full.find_last_of('/');
  if (std::string::npos == slash) {
    dir = ".";
    basename = full;
  } else {
    dir = full.substr(0, 0 == slash ? 1 : slash);
    basename = full.substr(slash + 1);
  }
  return ret;
}

} // namespace

FileScanOptions::FileScanOptions()
    : field_delim_(','), line_delim_('\n'), quote_('"'), escape_(0),
      has_header_(true), null_str_(), sample_size_(-1)
{
}

int FileSnapshot::capture(const char *path)
{
  int ret = OB_SUCCESS;
  struct stat file_stat;
  valid_ = false;
  if (OB_ISNULL(path) || 0 != stat(path, &file_stat) || !S_ISREG(file_stat.st_mode)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    size_ = static_cast<int64_t>(file_stat.st_size);
    dev_ = static_cast<int64_t>(file_stat.st_dev);
    ino_ = static_cast<int64_t>(file_stat.st_ino);
#ifdef _WIN32
    mtime_ns_ = static_cast<int64_t>(file_stat.st_mtime) * 1000000000;
#else
    mtime_ns_ = static_cast<int64_t>(file_stat.st_mtim.tv_sec) * 1000000000
                + static_cast<int64_t>(file_stat.st_mtim.tv_nsec);
#endif
    valid_ = true;
  }
  return ret;
}

int FileSnapshot::check(const char *path) const
{
  int ret = OB_SUCCESS;
  struct stat file_stat;
  if (!valid_) {
    ret = OB_STATE_NOT_MATCH;
  } else if (OB_ISNULL(path) || 0 != stat(path, &file_stat) || !S_ISREG(file_stat.st_mode)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    int64_t mtime_ns = 0;
#ifdef _WIN32
    mtime_ns = static_cast<int64_t>(file_stat.st_mtime) * 1000000000;
#else
    mtime_ns = static_cast<int64_t>(file_stat.st_mtim.tv_sec) * 1000000000
               + static_cast<int64_t>(file_stat.st_mtim.tv_nsec);
#endif
    if (static_cast<int64_t>(file_stat.st_size) != size_
        || static_cast<int64_t>(file_stat.st_dev) != dev_
        || static_cast<int64_t>(file_stat.st_ino) != ino_
        || mtime_ns != mtime_ns_) {
      ret = OB_STATE_NOT_MATCH;
    }
  }
  return ret;
}

int file_scan_infer_schema(const char *path,
                           const FileScanFormat format,
                           const FileScanOptions &options,
                           common::ObIAllocator &allocator,
                           common::ObIArray<FileScanColumn> &columns,
                           int64_t &row_count)
{
  int ret = OB_SUCCESS;
  std::vector<FsColumnState> scan_columns;
  row_count = 0;
  if (FILE_SCAN_FORMAT_CSV == format) {
    if (OB_FAIL(fs_infer_csv(path, options, scan_columns, row_count))) {
    }
  } else if (FILE_SCAN_FORMAT_JSONL == format) {
    if (OB_FAIL(fs_infer_jsonl(path, scan_columns, row_count))) {
    }
  } else if (FILE_SCAN_FORMAT_PARQUET == format) {
    if (OB_FAIL(fs_infer_parquet(path, scan_columns))) {
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(scan_columns.size()); ++i) {
    FileScanColumn column;
    if (OB_FAIL(ob_write_string(allocator,
                                ObString(static_cast<int32_t>(scan_columns[i].name_.size()),
                                         const_cast<char *>(scan_columns[i].name_.data())),
                                column.name_))) {
    } else {
      fs_scan_type_to_meta(scan_columns[i].type_, column.meta_, column.accuracy_);
      column.nullable_ = scan_columns[i].nullable_;
      if (OB_FAIL(columns.push_back(column))) {
      }
    }
  }
  return ret;
}

int file_scan_create_reader(const char *path,
                            const FileScanFormat format,
                            const FileScanOptions &options,
                            const common::ObIArray<int64_t> *projection,
                            FileScanReader *&reader)
{
  int ret = OB_SUCCESS;
  reader = nullptr;
  std::vector<int64_t> projection_vec;
  if (OB_NOT_NULL(projection)) {
    for (int64_t i = 0; i < projection->count(); ++i) {
      projection_vec.push_back(projection->at(i));
    }
  }
  if (FILE_SCAN_FORMAT_CSV == format) {
    FsCsvFileReader *csv_reader = nullptr;
    if (OB_ISNULL(csv_reader = new (std::nothrow) FsCsvFileReader())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(csv_reader->init(path, options))) {
      delete csv_reader;
      csv_reader = nullptr;
    } else {
      reader = csv_reader;
    }
  } else if (FILE_SCAN_FORMAT_JSONL == format) {
    FsJsonlFileReader *json_reader = nullptr;
    if (OB_ISNULL(json_reader = new (std::nothrow) FsJsonlFileReader())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(json_reader->init(path, options))) {
      delete json_reader;
      json_reader = nullptr;
    } else {
      reader = json_reader;
    }
  } else if (FILE_SCAN_FORMAT_PARQUET == format) {
    FsParquetFileReader *pq_reader = nullptr;
    if (OB_ISNULL(pq_reader = new (std::nothrow) FsParquetFileReader())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(pq_reader->init(path, options, projection_vec))) {
      delete pq_reader;
      pq_reader = nullptr;
    } else {
      reader = pq_reader;
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int file_scan_glob(const char *pattern,
                   common::ObIAllocator &allocator,
                   common::ObIArray<common::ObString> &files)
{
  int ret = OB_SUCCESS;
  std::string dir;
  std::string basename;
  if (OB_ISNULL(pattern) || '\0' == *pattern) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(fs_glob_parent_dir(pattern, dir, basename))) {
  } else {
    std::vector<std::string> matches;
    DIR *handle = opendir(dir.c_str());
    if (OB_ISNULL(handle)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open directory for glob", K(ret), K(dir.c_str()), K(errno));
    } else {
      struct dirent *entry = nullptr;
      while (OB_NOT_NULL(entry = readdir(handle))) {
        const char *name = entry->d_name;
        if (0 == strcmp(name, ".") || 0 == strcmp(name, "..")) {
          continue;
        }
        if (fs_glob_match(basename.c_str(), name)) {
          std::string full = dir;
          if (!full.empty() && '/' != full.back()) {
            full.push_back('/');
          }
          full.append(name);
          struct stat file_stat;
          if (0 == stat(full.c_str(), &file_stat) && S_ISREG(file_stat.st_mode)) {
            matches.push_back(full);
          }
        }
      }
      closedir(handle);
      std::sort(matches.begin(), matches.end());
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(matches.size()); ++i) {
        common::ObString value;
        if (OB_FAIL(ob_write_string(allocator,
                                    ObString(static_cast<int32_t>(matches[i].size()),
                                             const_cast<char *>(matches[i].data())),
                                    value))) {
        } else if (OB_FAIL(files.push_back(value))) {
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
