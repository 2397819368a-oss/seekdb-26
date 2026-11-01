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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/table_function/table_function.h"
#include "sql/table_function/file_table_function.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase {
namespace sql {
using namespace common;

namespace file_table_scan_impl {

// 执行期只读取绑定期固化下来的结构（options_），不再重新推断列，避免结构漂移。
class FileTableScan final : public ITableFunctionScan
{
public:
  explicit FileTableScan(const TableFunctionScanContext &context)
    : context_(context), definition_(), source_(nullptr), values_(), cells_(), row_(),
      opened_(false)
  {}
  ~FileTableScan() override { (void) close(); }
  int open() override
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(deserialize_file_table_definition(context_.options_, context_.allocator_, definition_))) {
    } else if (OB_FAIL(override_runtime_path())) {
    } else if (OB_FAIL(check_file_table_path(definition_.path_))) {
    } else if (OB_FAIL(create_file_table_row_source(definition_, context_.allocator_, source_))) {
    } else if (OB_ISNULL(source_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(source_->open())) {
    } else if (OB_FAIL(prepare_row())) {
    } else {
      opened_ = true;
    }
    return ret;
  }
  int rescan(const ObIArray<ObObj> &arguments) override
  {
    int ret = OB_SUCCESS;
    UNUSED(arguments);
    if (OB_FAIL(close())) {
    } else if (OB_FAIL(override_runtime_path())) {
    } else if (OB_FAIL(create_file_table_row_source(definition_, context_.allocator_, source_))) {
    } else if (OB_ISNULL(source_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(source_->open())) {
    } else if (OB_FAIL(prepare_row())) {
    } else {
      opened_ = true;
    }
    return ret;
  }
  int get_next_row(ObNewRow *&row) override
  {
    int ret = OB_SUCCESS;
    row = nullptr;
    if (!opened_) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(source_->next_row(values_))) {
    } else {
      // 按投影列号回填，列顺序由优化器给出，可能不是 schema 顺序。
      const int64_t count = context_.column_ids_.count();
      for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        const int64_t position = static_cast<int64_t>(context_.column_ids_.at(i)) - OB_APP_MIN_COLUMN_ID;
        if (position < 0 || position >= values_.count()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("projected column is out of the file schema", K(ret), K(position), K(values_.count()));
        } else {
          cells_.at(i) = values_.at(position);
        }
      }
      if (OB_SUCC(ret)) {
        row_.cells_ = cells_.get_data();
        row_.count_ = cells_.count();
        row = &row_;
      }
    }
    return ret;
  }
  int close() override
  {
    int ret = OB_SUCCESS;
    if (source_ != nullptr) {
      ret = source_->close();
    }
    opened_ = false;
    return ret;
  }
private:
  // 文件路径来自运行时参数（避免 plan cache 把不同路径的语句复用同一份固化定义）。
  // 列结构仍使用绑定期推断的结果，要求同一条语句执行期间文件 schema 不变。
  int override_runtime_path()
  {
    int ret = OB_SUCCESS;
    if (context_.arguments_.count() < 1) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path argument is missing");
    } else {
      const ObObj &arg = context_.arguments_.at(0);
      if (arg.is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path must not be NULL");
      } else if (!arg.is_varchar() && !arg.is_char()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path must be a string");
      } else {
        const ObString path = arg.get_varchar();
        if (OB_FAIL(file_table_copy_string(context_.allocator_, path.ptr(), path.length(),
                                           definition_.path_))) {
        }
      }
    }
    return ret;
  }
  int prepare_row()
  {
    int ret = OB_SUCCESS;
    values_.reuse();
    cells_.reuse();
    for (int64_t i = 0; OB_SUCC(ret) && i < definition_.columns_.count(); ++i) {
      OZ (values_.push_back(ObObj()));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < context_.column_ids_.count(); ++i) {
      OZ (cells_.push_back(ObObj()));
    }
    return ret;
  }
  TableFunctionScanContext context_;
  FileTableDefinition definition_;
  FileTableRowSource *source_;
  ObSEArray<ObObj, 16> values_;
  ObSEArray<ObObj, 16> cells_;
  ObNewRow row_;
  bool opened_;
};

} // namespace file_table_scan_impl

using namespace file_table_scan_impl;

int create_file_table_scan(const TableFunctionScanContext &context, ITableFunctionScan *&scan)
{
  int ret = OB_SUCCESS;
  scan = nullptr;
  void *memory = context.allocator_.alloc(sizeof(FileTableScan));
  if (OB_ISNULL(memory)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    scan = new (memory) FileTableScan(context);
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
