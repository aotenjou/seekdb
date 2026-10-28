/**
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

#define USING_LOG_PREFIX SERVER
#include "observer/virtual_table/ob_all_ai_column.h"
#include "share/schema/ob_schema_getter_guard.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace observer
{

int ObAllAiColumn::inner_get_next_row(common::ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  if (!start_to_read_) {
    if (OB_FAIL(fill_scanner())) {
    } else {
      start_to_read_ = true;
    }
  }
  if (OB_SUCC(ret) && start_to_read_) {
    if (OB_FAIL(scanner_it_.get_next_row(cur_row_))) {
      if (OB_ITER_END != ret) {
        SERVER_LOG(WARN, "failed to get next ai column row", K(ret));
      }
    } else {
      row = &cur_row_;
    }
  }
  return ret;
}

int ObAllAiColumn::fill_scanner()
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObTableSchema *, 64> table_schemas;
  if (OB_ISNULL(allocator_) || OB_ISNULL(schema_guard_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(schema_guard_->get_table_schemas_in_runtime(table_schemas))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < table_schemas.count(); ++i) {
      const ObTableSchema *table_schema = table_schemas.at(i);
      ObSEArray<const ObColumnSchemaV2 *, 8> ai_columns;
      if (OB_ISNULL(table_schema)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (ObTableSchema::const_column_iterator it = table_schema->column_begin();
             OB_SUCC(ret) && it != table_schema->column_end(); ++it) {
          const ObColumnSchemaV2 *column = *it;
          if (OB_ISNULL(column)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (column->is_ai_column()) {
            if (OB_FAIL(ai_columns.push_back(column))) {
            }
          }
        }
      }
      for (int64_t j = 0; OB_SUCC(ret) && j < ai_columns.count(); ++j) {
        const ObColumnSchemaV2 *column = ai_columns.at(j);
        char spec_name[192];
        snprintf(spec_name, sizeof(spec_name), "__ai_%.*s_sp",
                 static_cast<int>(column->get_column_name_str().length()),
                 column->get_column_name_str().ptr());
        const ObColumnSchemaV2 *spec_column = nullptr;
        char state_name[192];
        char request_name[192];
        snprintf(state_name, sizeof(state_name), "__ai_%.*s_st",
                 static_cast<int>(column->get_column_name_str().length()),
                 column->get_column_name_str().ptr());
        snprintf(request_name, sizeof(request_name), "__ai_%.*s_rq",
                 static_cast<int>(column->get_column_name_str().length()),
                 column->get_column_name_str().ptr());
        const ObColumnSchemaV2 *state_column = nullptr;
        const ObColumnSchemaV2 *request_column = nullptr;
        for (ObTableSchema::const_column_iterator it = table_schema->column_begin();
             it != table_schema->column_end(); ++it) {
          const ObColumnSchemaV2 *candidate = *it;
          if (OB_ISNULL(candidate)) {
            continue;
          }
          if (nullptr == spec_column
              && 0 == candidate->get_column_name_str().case_compare(ObString::make_string(spec_name))) {
            spec_column = candidate;
          } else if (nullptr == state_column
              && 0 == candidate->get_column_name_str().case_compare(ObString::make_string(state_name))) {
            state_column = candidate;
          } else if (nullptr == request_column
              && 0 == candidate->get_column_name_str().case_compare(ObString::make_string(request_name))) {
            request_column = candidate;
          }
        }
        if (OB_NOT_NULL(spec_column)) {
          if (OB_FAIL(add_ai_column_row(*table_schema, *column, *spec_column,
                                        state_column, request_column))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      scanner_it_ = scanner_.begin();
    }
  }
  return ret;
}

int ObAllAiColumn::add_ai_column_row(const ObTableSchema &table_schema,
                                     const ObColumnSchemaV2 &column,
                                     const ObColumnSchemaV2 &spec_column,
                                     const ObColumnSchemaV2 *state_column,
                                     const ObColumnSchemaV2 *request_column)
{
  int ret = OB_SUCCESS;
  ObObj *cells = cur_row_.cells_;
  ObString comment = spec_column.get_comment_str();
  int64_t max_retries = 0;
  ObString normalized_expr;
  if (OB_ISNULL(cells)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // Metadata layout: max_retries\x01normalized_expr\x01...
    const char *first = static_cast<const char *>(memchr(comment.ptr(), '\x01', comment.length()));
    if (OB_ISNULL(first)) {
      normalized_expr = comment;
    } else {
      const int64_t meta_len = first - comment.ptr();
      char meta_buf[64];
      const int64_t copy_len = MIN(meta_len, static_cast<int64_t>(sizeof(meta_buf) - 1));
      MEMCPY(meta_buf, comment.ptr(), copy_len);
      meta_buf[copy_len] = '\0';
      max_retries = strtoll(meta_buf, nullptr, 10);
      const char *second = static_cast<const char *>(
          memchr(first + 1, '\x01', comment.length() - meta_len - 1));
      if (OB_ISNULL(second)) {
        normalized_expr.assign_ptr(first + 1,
                                   static_cast<int32_t>(comment.length() - meta_len - 1));
      } else {
        normalized_expr.assign_ptr(first + 1,
                                   static_cast<int32_t>(second - (first + 1)));
      }
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < output_column_ids_.count(); ++i) {
      const uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case TABLE_ID:
          cells[i].set_int(static_cast<int64_t>(table_schema.get_table_id()));
          break;
        case COLUMN_ID:
          cells[i].set_int(static_cast<int64_t>(column.get_column_id()));
          break;
        case COLUMN_NAME:
          cells[i].set_varchar(column.get_column_name_str());
          cells[i].set_collation_type(CS_TYPE_UTF8MB4_BIN);
          break;
        case NORMALIZED_EXPR:
          cells[i].set_varchar(normalized_expr);
          cells[i].set_collation_type(CS_TYPE_UTF8MB4_BIN);
          break;
        case MAX_RETRIES:
          cells[i].set_int(max_retries);
          break;
        case SYNC_MODE:
          cells[i].set_varchar(ObString::make_string("async"));
          cells[i].set_collation_type(CS_TYPE_UTF8MB4_BIN);
          break;
        case STATE_COLUMN_ID:
          cells[i].set_int(nullptr == state_column ? 0 : static_cast<int64_t>(state_column->get_column_id()));
          break;
        case REQUEST_COLUMN_ID:
          cells[i].set_int(nullptr == request_column ? 0 : static_cast<int64_t>(request_column->get_column_id()));
          break;
        case RESULT_SPEC_COLUMN_ID:
          cells[i].set_int(static_cast<int64_t>(spec_column.get_column_id()));
          break;
        case DEFINITION_VERSION:
          cells[i].set_int(1);
          break;
        default:
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "unexpected column id", K(ret), K(col_id));
          break;
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(scanner_.add_row(cur_row_))) {
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
