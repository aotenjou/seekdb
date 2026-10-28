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

#ifndef OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_AI_COLUMN_
#define OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_AI_COLUMN_

#include "observer/virtual_table/ob_virtual_table_scanner_iterator.h"

namespace oceanbase
{
namespace observer
{
// Exposes AI COLUMN definitions derived from the schema. The result column and
// its three hidden companion columns carry all the persisted metadata, so no
// extra inner table rows are required for DDL.
class ObAllAiColumn : public common::ObVirtualTableScannerIterator
{
public:
  ObAllAiColumn() {}
  virtual ~ObAllAiColumn() { reset(); }
  virtual void reset() override { ObVirtualTableScannerIterator::reset(); }
  virtual int inner_get_next_row(common::ObNewRow *&row) override;
private:
  enum AI_COLUMN_COLUMN
  {
    TABLE_ID = common::OB_APP_MIN_COLUMN_ID,
    COLUMN_ID,
    COLUMN_NAME,
    NORMALIZED_EXPR,
    MAX_RETRIES,
    SYNC_MODE,
    STATE_COLUMN_ID,
    REQUEST_COLUMN_ID,
    RESULT_SPEC_COLUMN_ID,
    DEFINITION_VERSION,
  };
  int fill_scanner();
  int add_ai_column_row(const share::schema::ObTableSchema &table_schema,
                        const share::schema::ObColumnSchemaV2 &column,
                        const share::schema::ObColumnSchemaV2 &spec_column,
                        const share::schema::ObColumnSchemaV2 *state_column,
                        const share::schema::ObColumnSchemaV2 *request_column);
  ObAllAiColumn(const ObAllAiColumn &other) = delete;
  ObAllAiColumn &operator=(const ObAllAiColumn &other) = delete;
};
} // namespace observer
} // namespace oceanbase
#endif
