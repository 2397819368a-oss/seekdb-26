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

#define USING_LOG_PREFIX SERVER

#include "observer/virtual_table/ob_all_virtual_resource_limit.h"
#include "share/rc/ob_server_runtime.h"
#include "share/cache/ob_kv_storecache.h"
#include "sql/engine/ob_sql_memory_manager.h"
#include "storage/allocator/ob_shared_memory_allocator_mgr.h"
#include "storage/meta_mem/ob_storage_meta_mem_mgr.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::share;
namespace oceanbase
{
namespace observer
{

ObResourceLimitTable::ObResourceLimitTable()
  : ObVirtualTableScannerIterator(),
    iter_(),
    logical_resources_finished_(false),
    memory_resource_index_(0)
{
}

ObResourceLimitTable::~ObResourceLimitTable()
{
  reset();
}

void ObResourceLimitTable::reset()
{
  iter_.reset();
  logical_resources_finished_ = false;
  memory_resource_index_ = 0;
  ObVirtualTableScannerIterator::reset();
}

const char *ObResourceLimitTable::memory_resource_name(const int64_t index)
{
  static const char *const NAMES[] = {
      "kv_cache_memory", "sql_workarea_memory", "vector_memory", "meta_object_memory"};
  return index >= 0 && index < memory_resource_count() ? NAMES[index] : nullptr;
}

int ObResourceLimitTable::get_memory_resource_sample(
    const int64_t index, MemoryResourceSample &sample)
{
  int ret = OB_SUCCESS;
  MemoryQuotaSample quota_sample;
  sample = MemoryResourceSample();
  switch (index) {
    case 0:
      quota_sample = ObKVGlobalCache::get_instance().get_memory_quota_sample();
      break;
    case 1: {
      sql::ObSqlMemoryManager *manager = server_service<sql::ObSqlMemoryManager>();
      if (OB_ISNULL(manager)) {
        ret = OB_NOT_INIT;
      } else {
        sample.used_bytes_ = manager->get_total_mem_used();
        sample.limit_bytes_ = manager->get_max_workarea_size();
        sample.is_scheduling_budget_ = true;
      }
      break;
    }
    case 2: {
      ObSharedMemAllocMgr *manager = server_service<ObSharedMemAllocMgr>();
      if (OB_ISNULL(manager)) {
        ret = OB_NOT_INIT;
      } else {
        quota_sample = manager->vector_allocator().get_memory_quota_sample();
      }
      break;
    }
    case 3: {
      ObStorageMetaMemMgr *manager = server_service<ObStorageMetaMemMgr>();
      if (OB_ISNULL(manager)) {
        ret = OB_NOT_INIT;
      } else {
        sample.used_bytes_ = manager->get_memory_used();
        sample.is_usage_only_ = true;
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
      break;
  }
  if (OB_SUCC(ret) && !sample.is_scheduling_budget_ && !sample.is_usage_only_) {
    sample.used_bytes_ = quota_sample.committed_bytes_;
    sample.reserved_bytes_ = quota_sample.reserved_bytes_;
    sample.limit_bytes_ = quota_sample.limit_bytes_;
  }
  return ret;
}

int ObResourceLimitTable::get_next_resource_info_(ObResourceInfo &info)
{
  int ret = OB_SUCCESS;
  if (!iter_.is_ready()
      && OB_FAIL(iter_.set_ready(*::oceanbase::share::server_service<::oceanbase::share::ObResourceLimitCalculator>()))) {
  } else if (OB_FAIL(iter_.get_next(info))) {
    if (OB_ITER_END != ret) {
    }
  }
  return ret;
}

int ObResourceLimitTable::inner_get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  ObResourceInfo info;
  MemoryResourceSample sample;
  row = nullptr;
  if (OB_ISNULL(allocator_) || OB_ISNULL(cur_row_.cells_)) {
    ret = OB_NOT_INIT;
  } else {
    start_to_read_ = true;
    if (!logical_resources_finished_ && OB_FAIL(get_next_resource_info_(info))) {
      if (OB_ITER_END == ret) {
        logical_resources_finished_ = true;
        ret = OB_SUCCESS;
      }
    }
    if (OB_SUCC(ret) && logical_resources_finished_) {
      if (memory_resource_index_ >= memory_resource_count()) {
        ret = OB_ITER_END;
      } else if (OB_FAIL(get_memory_resource_sample(memory_resource_index_, sample))) {
      } else {
        // Independent observations, not a joint snapshot. WorkArea reports
        // profile usage and a scheduling budget rather than quota admission.
        info.curr_utilization_ = sample.used_bytes_;
        info.reserved_value_ = sample.reserved_bytes_;
        info.min_constraint_value_ = sample.limit_bytes_;
        info.min_constraint_type_ = MEMORY_CONSTRAINT;
      }
    }
  }
  if (OB_SUCC(ret)) {
    const int64_t col_count = output_column_ids_.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case RESOURCE_NAME: {
          cur_row_.cells_[i].set_varchar(logical_resources_finished_
              ? memory_resource_name(memory_resource_index_)
              : get_logic_res_type_name(iter_.get_curr_type()));
          cur_row_.cells_[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        }
        case CURRENT_UTILIZATION: {
          cur_row_.cells_[i].set_int(info.curr_utilization_);
          break;
        }
        case MAX_UTILIZATION:
          if (logical_resources_finished_) {
            // No memory high-water counter is maintained on allocation paths.
            cur_row_.cells_[i].set_null();
          } else {
            cur_row_.cells_[i].set_int(info.max_utilization_);
          }
          break;
        case RSERVED_VALUE:
          if (logical_resources_finished_ && (sample.is_scheduling_budget_ || sample.is_usage_only_)) {
            cur_row_.cells_[i].set_null();
          } else {
            cur_row_.cells_[i].set_int(info.reserved_value_);
          }
          break;
        case LIMIT_VALUE:
          if (logical_resources_finished_ && sample.is_usage_only_) {
            cur_row_.cells_[i].set_null();
          } else {
            cur_row_.cells_[i].set_int(info.min_constraint_value_);
          }
          break;
        case EFFECTIVE_LIMIT_TYPE:
          cur_row_.cells_[i].set_varchar(
              logical_resources_finished_ && sample.is_usage_only_ ? "none"
                  : logical_resources_finished_ && sample.is_scheduling_budget_
                      ? "scheduling" : get_constraint_type_name(info.min_constraint_type_));
          cur_row_.cells_[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          break;
        default:
          ret = OB_ERR_UNEXPECTED;
          break;
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (logical_resources_finished_) {
      ++memory_resource_index_;
    }
    row = &cur_row_;
  }
  return ret;
}

} // end namespace observer
} // end namespace oceanbase
