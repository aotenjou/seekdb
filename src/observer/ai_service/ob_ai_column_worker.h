#ifndef OBDEV_SRC_OBSERVER_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_
#define OBDEV_SRC_OBSERVER_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_iarray.h"

namespace oceanbase
{
namespace sql
{

class ObAiColumnWorker
{
public:
  struct TaskInfo
  {
    TaskInfo() : table_id_(0), column_id_(0), pred_(), request_id_(), state_(0), retry_count_(0),
                 eligible_ts_(0), model_(), prompt_(), instruction_(), result_type_(1),
                 max_retries_(0), definition_version_(1) {}
    uint64_t table_id_;
    uint64_t column_id_;
    common::ObString pred_;
    common::ObString request_id_;
    int64_t state_;
    int64_t retry_count_;
    int64_t eligible_ts_;
    common::ObString model_;
    common::ObString prompt_;
    common::ObString instruction_;
    int64_t result_type_;
    int64_t max_retries_;
    int64_t definition_version_;
  };

  static int add_task(uint64_t table_id, const common::ObString &pred, uint64_t column_id,
                      const common::ObString &request_id, const common::ObString &model,
                      const common::ObString &prompt_expr, const common::ObString &instruction,
                      int64_t result_type, int64_t max_retries, uint64_t session_id);
  static void remove_tasks(uint64_t table_id, const common::ObString &pred,
                           const common::ObString &request_id);
  static void remove_session_tasks(uint64_t session_id);
  static int get_tasks(common::ObIArray<TaskInfo> &tasks);

  // Column completeness bookkeeping used by strict reads.
  static void mark_incomplete(uint64_t table_id, uint64_t column_id);
  static void mark_complete(uint64_t table_id, uint64_t column_id);
  static bool is_incomplete(uint64_t table_id, uint64_t column_id);
  static bool has_unfinished_columns();
  static int check_ai_column_readable(uint64_t table_id, uint64_t column_id);
  static int process_pending();
private:
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnWorker);
};

} // namespace sql
} // namespace oceanbase

#endif /* OBDEV_SRC_OBSERVER_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_ */
