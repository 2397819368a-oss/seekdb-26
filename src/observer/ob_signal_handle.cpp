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

#if defined(__APPLE__) && defined(__MACH__)
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

static int sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout) {
  (void)info;
  (void)timeout;

  int signum = 0;
  int result = sigwait(const_cast<sigset_t*>(set), &signum);

  if (result != 0) {
    errno = result;
    return -1;
  }
  return signum;
}
#endif

#include "ob_signal_handle.h"
#include <cstdio>
#if defined(_WIN32)
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif
#include "lib/allocator/ob_jemalloc.h"
#include "lib/utility/ob_platform_utils.h"
#include "observer/ob_server.h"
#include "sql/ob_sql_init.h"
#include "storage/tx_storage/ob_memory_printer.h"

extern void switch_check_io_hang_errsim();

namespace oceanbase
{
using namespace common;
using namespace storage;

namespace observer
{

namespace
{

struct AllocatorProcessStats
{
  AllocatorProcessStats()
    : allocator_stats_available_(false), allocated_(0), active_(0),
      resident_(0), mapped_(0), virtual_memory_(0), process_resident_(0)
  {}

  bool allocator_stats_available_;
  uint64_t allocated_;
  uint64_t active_;
  uint64_t resident_;
  uint64_t mapped_;
  int64_t virtual_memory_;
  int64_t process_resident_;
};

int64_t get_virtual_memory_used(int64_t *resident_size)
{
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    if (nullptr != resident_size) {
      *resident_size = static_cast<int64_t>(counters.WorkingSetSize);
    }
    return static_cast<int64_t>(counters.PagefileUsage);
  }
  if (nullptr != resident_size) {
    *resident_size = 0;
  }
  return 0;
#elif defined(__APPLE__)
  mach_task_basic_info_data_t task_info_value = {};
  mach_msg_type_number_t task_info_count = MACH_TASK_BASIC_INFO_COUNT;
  if (KERN_SUCCESS == task_info(
          mach_task_self(), MACH_TASK_BASIC_INFO,
          reinterpret_cast<task_info_t>(&task_info_value),
          &task_info_count)) {
    if (nullptr != resident_size) {
      *resident_size = static_cast<int64_t>(task_info_value.resident_size);
    }
    return static_cast<int64_t>(task_info_value.virtual_size);
  }
  if (nullptr != resident_size) {
    *resident_size = 0;
  }
  return 0;
#else
  static const ssize_t page_size = lib::ob_get_page_size();
  int64_t page_count = 0;
  int64_t resident_page_count = 0;
  FILE *statm = fopen("/proc/self/statm", "r");
  if (nullptr != statm) {
    static_cast<void>(fscanf(statm, "%ld %ld", &page_count,
                             &resident_page_count));
    fclose(statm);
  }
  if (nullptr != resident_size) {
    *resident_size = resident_page_count * page_size;
  }
  return page_count * page_size;
#endif
}

bool get_allocator_process_stats(AllocatorProcessStats &stats)
{
  stats = AllocatorProcessStats();
  stats.virtual_memory_ = get_virtual_memory_used(&stats.process_resident_);
#if defined(OB_HAVE_BUNDLED_JEMALLOC)
  uint64_t epoch = 1;
  size_t epoch_size = sizeof(epoch);
  size_t value_size = sizeof(size_t);
  size_t allocated = 0;
  size_t active = 0;
  size_t resident = 0;
  size_t mapped = 0;
  if (0 == je_mallctl("epoch", &epoch, &epoch_size, &epoch, sizeof(epoch))
      && 0 == je_mallctl("stats.allocated", &allocated, &value_size, nullptr, 0)
      && (value_size = sizeof(size_t),
          0 == je_mallctl("stats.active", &active, &value_size, nullptr, 0))
      && (value_size = sizeof(size_t),
          0 == je_mallctl("stats.resident", &resident, &value_size, nullptr, 0))
      && (value_size = sizeof(size_t),
          0 == je_mallctl("stats.mapped", &mapped, &value_size, nullptr, 0))) {
    stats.allocator_stats_available_ = true;
    stats.allocated_ = static_cast<uint64_t>(allocated);
    stats.active_ = static_cast<uint64_t>(active);
    stats.resident_ = static_cast<uint64_t>(resident);
    stats.mapped_ = static_cast<uint64_t>(mapped);
  }
#endif
  return stats.allocator_stats_available_;
}

void ob_print_mod_memory_usage(bool print_to_std = false,
                               bool print_glibc_malloc_stats = false)
{
  UNUSED(print_glibc_malloc_stats);
  AllocatorProcessStats stats;
  const bool available = get_allocator_process_stats(stats);
  const uint64_t allocated = stats.allocated_;
  const uint64_t active = stats.active_;
  const uint64_t allocator_resident = stats.resident_;
  const uint64_t mapped = stats.mapped_;
  const int64_t virtual_memory = stats.virtual_memory_;
  const int64_t process_resident = stats.process_resident_;
  LIB_LOG(INFO, "allocator process memory", K(available), K(allocated),
          K(active), K(allocator_resident), K(mapped), K(virtual_memory),
          K(process_resident));
  if (print_to_std) {
    fprintf(stderr,
            "allocator_process_memory available=%d allocated=%llu active=%llu "
            "allocator_resident=%llu mapped=%llu virtual_memory=%lld "
            "process_resident=%lld\n",
            available ? 1 : 0, static_cast<unsigned long long>(allocated),
            static_cast<unsigned long long>(active),
            static_cast<unsigned long long>(allocator_resident),
            static_cast<unsigned long long>(mapped),
            static_cast<long long>(virtual_memory),
            static_cast<long long>(process_resident));
  }
}

} // namespace

void ObSignalHandle::run1()
{
  // Save thread id for macOS wakeup in destructor
  thread_id_ = pthread_self();

  int ret = OB_SUCCESS;
  lib::set_thread_name("SignalHandle");
  sigset_t   waitset;
  if (OB_FAIL(add_signums_to_set(waitset))) {
  } else {
    int signum = -1;
    //to check _stop every second
    struct timespec timeout = {1, 0};
    while (!has_set_stop()) {//need not to check ret
      {
        signum = sigtimedwait(&waitset, NULL, &timeout);
      }
      if (-1 == signum) {
        //do not log error, because timeout will also return -1.
      } else if (OB_FAIL(deal_signals(signum))) {
      } else {
        //do nothing
      }
    }
  }
}

int ObSignalHandle::change_signal_mask()
{
  int ret = OB_SUCCESS;
  sigset_t block_set, old_set;
  if (OB_FAIL(add_signums_to_set(block_set))) {
  } else if (0 != pthread_sigmask(SIG_BLOCK, &block_set, &old_set)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("Fail to change mask of blcked signals", K(ret));
  } else {
    //do nothing
  }
  return ret;
}

int ObSignalHandle::add_signums_to_set(sigset_t &sig_set)
{
  int ret = OB_SUCCESS;
  if (0 != sigemptyset(&sig_set)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("Empty signal set error", K(ret));
  } else {
    int rc = 0;
    int signals[] = {SIGPIPE, SIGTERM, SIGUSR1,
                     40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
                     50, 51, 52, 53, 55, 56, 57, 59, 60,
                     63, 64};
    for (int64_t i = 0; OB_SUCC(ret) && i < ARRAYSIZEOF(signals); ++i) {
      if (0 != (rc = sigaddset(&sig_set, signals[i]))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_ERROR("failed to add signal to block", "signum", signals[i], K(ret), K(rc));
      }
    }
  }
  return ret;
}

//signal http://blog.csdn.net/yiyeguzhou100/article/details/51316175
int ObSignalHandle::deal_signals(int signum)
{
  int ret = OB_SUCCESS;
  LOG_INFO("received signal", K(signum));
  switch (signum)
  {
    case SIGPIPE: {
      break;
    }
    case SIGTERM: {
#ifdef _WIN32
      TerminateProcess(GetCurrentProcess(), 1);
#else
      raise(SIGKILL);
#endif
      break;
    }
    case SIGUSR1: {
      ObServer::get_instance().prepare_stop();
      ObServer::get_instance().set_stop();
      break;
    }
    case 40: {
      OB_LOGGER.check_file();
      break;
    }
    case 41: {
      int64_t version = ::oceanbase::common::ObTimeUtility::current_time();
      OB_LOGGER.down_log_level(version);
      // ASYNC_LOG_LOGGER.down_log_level();
      //to echo info when change to warn level
      LOG_WARN("Signal 41 down ObLogger level", "current level", OB_LOGGER.get_level());
      break;
    }
    case 42: {
      int64_t version = ::oceanbase::common::ObTimeUtility::current_time();
      OB_LOGGER.up_log_level(version);
      // ASYNC_LOG_LOGGER.up_log_level();
      LOG_WARN("Signal 42 up ObLogger level", "current level", OB_LOGGER.get_level());
      break;
    }
    case 43: {
      break;
    }
    case 45: {
      break;
    }
    case 46: {
      break;
    }
    case 49: {
      ob_print_mod_memory_usage();
      ObMemoryPrinter::get_instance().print_memory_usage();
      switch_check_io_hang_errsim();
      break;
    }
    case 50: {
      break;
    }
    case 51: {
      break;
    }
    case 53: {
      // debug SQL modules
      if (OB_FAIL(
          ObLogger::get_logger().set_mod_log_levels("ALL.*:ERROR, SQL.*:DEBUG, RPC.*:WARN"))) {
      }
      break;
    }
    case 57: {
      // clog::ObBatchSubmitMockTest::handle_signal();
      break;
    }
    case 59: {               // similar to 49, verbose mode
      ob_print_mod_memory_usage(false, true);
      sql::print_sql_stat();
      break;
    }
    case 60: {
      break;
    }
    case 63: {
      // Print runtime memstore consumption.
      ObMemoryPrinter::get_instance().print_memory_usage();
      break;
    }
    default: {
      LOG_WARN("Ignore unknown signal", K(signum));
      break;
    }
  }
  return ret;
}

}
}
