#ifndef OBDEV_SRC_SQL_ENGINE_DML_OB_AI_COLUMN_DML_HELPER_H_
#define OBDEV_SRC_SQL_ENGINE_DML_OB_AI_COLUMN_DML_HELPER_H_

#include "sql/engine/dml/ob_table_modify_op.h"

namespace oceanbase
{
namespace sql
{

class ObAiColumnDmlHelper
{
public:
  enum Event
  {
    AI_EVENT_INSERT = 0,
    AI_EVENT_UPDATE = 1,
    AI_EVENT_DELETE = 2,
  };
  // Record one modified row. The row datums are aligned with column_ids.
  static int record_row(ObTableModifyOp &op,
                        uint64_t table_id,
                        const ObIArray<uint64_t> &column_ids,
                        const ObChunkDatumStore::StoredRow *row,
                        Event event,
                        const ObChunkDatumStore::StoredRow *old_row,
                        const ObIArray<uint64_t> *updated_column_ids);
  static int record_row(ObTableModifyOp &op,
                        uint64_t table_id,
                        const ObIArray<uint64_t> &column_ids,
                        const ExprFixedArray &row,
                        Event event,
                        const ExprFixedArray *old_row,
                        const ObIArray<uint64_t> *updated_column_ids);
  // Write invalidation updates and task rows through a nested session so that
  // they belong to the same transaction as the outer DML.
  static int flush(ObTableModifyOp &op);
  static void reset();
  static void set_internal_write(bool value);
private:
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnDmlHelper);
};

} // namespace sql
} // namespace oceanbase

#endif /* OBDEV_SRC_SQL_ENGINE_DML_OB_AI_COLUMN_DML_HELPER_H_ */
