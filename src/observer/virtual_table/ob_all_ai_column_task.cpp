#define USING_LOG_PREFIX SERVER
#include "observer/virtual_table/ob_all_ai_column_task.h"
#include "observer/ai_service/ob_ai_column_worker.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace observer
{

int ObAllAiColumnTask::inner_get_next_row(common::ObNewRow *&row)
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
        SERVER_LOG(WARN, "failed to get next ai column task row", K(ret));
      }
    } else {
      row = &cur_row_;
    }
  }
  return ret;
}

int ObAllAiColumnTask::fill_scanner()
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAiColumnWorker::TaskInfo, 16> tasks;
  if (OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(ObAiColumnWorker::get_tasks(tasks))) {
  } else {
    for (int64_t r = 0; OB_SUCC(ret) && r < tasks.count(); ++r) {
      const ObAiColumnWorker::TaskInfo &task = tasks.at(r);
      ObObj *cells = cur_row_.cells_;
      if (OB_ISNULL(cells)) {
        ret = OB_ERR_UNEXPECTED;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < output_column_ids_.count(); ++i) {
        const uint64_t col_id = output_column_ids_.at(i);
        switch (col_id) {
          case TABLE_ID:
            cells[i].set_int(static_cast<int64_t>(task.table_id_));
            break;
          case ROWKEY_PREDICATE:
            cells[i].set_varchar(task.pred_);
            cells[i].set_collation_type(CS_TYPE_UTF8MB4_BIN);
            break;
          case COLUMN_ID:
            cells[i].set_int(static_cast<int64_t>(task.column_id_));
            break;
          case REQUEST_ID:
            cells[i].set_varchar(task.request_id_);
            cells[i].set_collation_type(CS_TYPE_UTF8MB4_BIN);
            break;
          case STATE:
            cells[i].set_int(task.state_);
            break;
          case RETRY_COUNT:
            cells[i].set_int(task.retry_count_);
            break;
          case ELIGIBLE_TS:
            cells[i].set_timestamp(task.eligible_ts_);
            break;
          case MODEL_NAME:
            cells[i].set_varchar(task.model_);
            cells[i].set_collation_type(CS_TYPE_UTF8MB4_BIN);
            break;
          case PROMPT_EXPR:
            cells[i].set_varchar(task.prompt_);
            cells[i].set_collation_type(CS_TYPE_UTF8MB4_BIN);
            break;
          case INSTRUCTION:
            cells[i].set_varchar(task.instruction_);
            cells[i].set_collation_type(CS_TYPE_UTF8MB4_BIN);
            break;
          case RESULT_TYPE:
            cells[i].set_int(task.result_type_);
            break;
          case MAX_RETRIES:
            cells[i].set_int(task.max_retries_);
            break;
          case DEFINITION_VERSION:
            cells[i].set_int(task.definition_version_);
            break;
          default:
            ret = OB_ERR_UNEXPECTED;
            SERVER_LOG(WARN, "unexpected column id", K(ret), K(col_id));
            break;
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(scanner_.add_row(cur_row_))) {
      }
    }
    if (OB_SUCC(ret)) {
      scanner_it_ = scanner_.begin();
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
