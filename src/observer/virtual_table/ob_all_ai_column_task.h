#ifndef OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_AI_COLUMN_TASK_
#define OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_AI_COLUMN_TASK_

#include "observer/virtual_table/ob_virtual_table_scanner_iterator.h"

namespace oceanbase
{
namespace observer
{
class ObAllAiColumnTask : public common::ObVirtualTableScannerIterator
{
public:
  ObAllAiColumnTask() {}
  virtual ~ObAllAiColumnTask() { reset(); }
  virtual void reset() override { ObVirtualTableScannerIterator::reset(); }
  virtual int inner_get_next_row(common::ObNewRow *&row) override;
private:
  enum TASK_COLUMN
  {
    TABLE_ID = common::OB_APP_MIN_COLUMN_ID,
    ROWKEY_PREDICATE,
    COLUMN_ID,
    REQUEST_ID,
    STATE,
    RETRY_COUNT,
    ELIGIBLE_TS,
    MODEL_NAME,
    PROMPT_EXPR,
    INSTRUCTION,
    RESULT_TYPE,
    MAX_RETRIES,
    DEFINITION_VERSION,
  };
  int fill_scanner();
  ObAllAiColumnTask(const ObAllAiColumnTask &other) = delete;
  ObAllAiColumnTask &operator=(const ObAllAiColumnTask &other) = delete;
};
} // namespace observer
} // namespace oceanbase
#endif
