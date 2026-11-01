/**
 * AI column background worker.
 *
 * It periodically scans __all_ai_column_task, computes the pending AI columns
 * of one row as a single atomic group (one request id per row) and writes the
 * results back to the user table.
 */
#ifndef OCEANBASE_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_
#define OCEANBASE_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_

#include "lib/task/ob_timer.h"
#include "lib/ob_define.h"
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase
{
namespace observer
{

struct AiColTaskItem
{
  uint64_t table_id_;
  int64_t column_id_;
  int64_t retry_count_;
  common::ObString column_name_;
  common::ObString rowkey_predicate_;
  common::ObString request_id_;
};

class ObAiColumnWorkerTask : public common::ObTimerTask
{
public:
  ObAiColumnWorkerTask();
  virtual ~ObAiColumnWorkerTask();
  int init(common::ObTimer &timer);
  void stop();
  virtual void runTimerTask() override;

private:
  int process_pending_tasks();
  int process_one_group(common::ObArenaAllocator &allocator,
                        const common::ObIArray<AiColTaskItem> &tasks,
                        const int64_t start,
                        const int64_t end,
                        const uint64_t table_id,
                        const common::ObString &request_id,
                        const common::ObString &predicate);
private:
  common::ObTimer *timer_;
  bool is_inited_;
  bool is_stopped_;
};

} // namespace observer
} // namespace oceanbase
#endif // OCEANBASE_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_
