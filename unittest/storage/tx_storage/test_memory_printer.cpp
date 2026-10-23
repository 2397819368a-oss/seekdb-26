/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#include "gtest/gtest.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/api/storage/runtime/ob_i_server_runtime.h"
#include "storage/tx_storage/ob_memory_printer.h"
#include "storage/tx_storage/ob_memstore_freezer.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::storage;

namespace
{

// The printer only checks runtime presence; no storage runtime operation is
// needed to exercise a missing or uninitialized MemStore statistics provider.
class MemoryPrinterRuntime final : public ObIServerRuntime
{
public:
  int create_runtime(const oceanbase::omt::ObServerRuntimeMeta &) override
  { return OB_NOT_SUPPORTED; }
  void set_synced() override {}
  bool has_runtime() const override { return false; }
  int get_server_log_disk_size(int64_t &size) override
  { size = 0; return OB_NOT_SUPPORTED; }
  bool is_hidden() override { return false; }
};

class MemoryPrinterServicesGuard final
{
public:
  MemoryPrinterServicesGuard(ObIServerRuntime *runtime, ObMemstoreFreezer *freezer)
      : previous_runtime_(server_service<ObIServerRuntime>()),
        previous_freezer_(server_service<ObMemstoreFreezer>()),
        previous_modules_ready_(g_server_modules_ready)
  {
    bind_server_service<ObIServerRuntime>(runtime);
    bind_server_service<ObMemstoreFreezer>(freezer);
    g_server_modules_ready = true;
  }

  ~MemoryPrinterServicesGuard()
  {
    g_server_modules_ready = previous_modules_ready_;
    bind_server_service<ObMemstoreFreezer>(previous_freezer_);
    bind_server_service<ObIServerRuntime>(previous_runtime_);
  }

private:
  ObIServerRuntime *previous_runtime_;
  ObMemstoreFreezer *previous_freezer_;
  bool previous_modules_ready_;
};

} // namespace

TEST(TestMemoryPrinter, missing_runtime_is_not_initialized)
{
  MemoryPrinterServicesGuard services(nullptr, nullptr);
  EXPECT_EQ(OB_NOT_INIT, ObMemoryPrinter::get_instance().print_memory_usage());
}

TEST(TestMemoryPrinter, missing_freezer_error_is_propagated_and_lock_is_released)
{
  MemoryPrinterRuntime runtime;
  MemoryPrinterServicesGuard services(&runtime, nullptr);
  auto &printer = ObMemoryPrinter::get_instance();
  EXPECT_EQ(OB_ERR_UNEXPECTED, printer.print_memory_usage());
  EXPECT_EQ(OB_ERR_UNEXPECTED, printer.print_memory_usage());
}

TEST(TestMemoryPrinter, unready_modules_do_not_report_success)
{
  MemoryPrinterRuntime runtime;
  MemoryPrinterServicesGuard services(&runtime, nullptr);
  g_server_modules_ready = false;
  EXPECT_EQ(OB_NOT_INIT, ObMemoryPrinter::get_instance().print_memory_usage());
}

TEST(TestMemoryPrinter, uninitialized_freezer_error_is_propagated_and_lock_is_released)
{
  MemoryPrinterRuntime runtime;
  ObMemstoreFreezer freezer;
  MemoryPrinterServicesGuard services(&runtime, &freezer);
  auto &printer = ObMemoryPrinter::get_instance();
  EXPECT_EQ(OB_NOT_INIT, printer.print_memory_usage());
  EXPECT_EQ(OB_NOT_INIT, printer.print_memory_usage());
}
