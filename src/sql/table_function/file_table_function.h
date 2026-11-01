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

#pragma once

#include "common/object/ob_object.h"
#include "lib/container/ob_iarray.h"
#include "lib/container/ob_se_array.h"

struct _ParseNode;

namespace oceanbase {
namespace sql {

class ObExecContext;
class ITableFunctionScan;
struct TableFunctionScanContext;
struct TableItem;
struct TableFunctionCall;

// read_csv / read_json(ndjson) / read_parquet 共享同一套“文件即表”描述：
// 绑定阶段推断列结构，执行阶段按推断结果扫描数据文件。
enum FileTableKind : int32_t {
  FILE_TABLE_CSV = 0,
  FILE_TABLE_JSON = 1,
  FILE_TABLE_PARQUET = 2,
  FILE_TABLE_GLOB = 3,
};

struct FileTableColumn
{
  common::ObString name_;
  common::ObObjType type_;
  int32_t length_;      // VARCHAR 的精度，其它类型为 0
  bool nullable_;
  FileTableColumn() : name_(), type_(common::ObNullType), length_(0), nullable_(true) {}
};

struct FileTableDefinition
{
  FileTableKind kind_;
  common::ObString path_;       // 绑定期字面量路径，支持 glob
  common::ObString delimiter_;  // csv 列分隔符，默认 ','
  common::ObString quote_;      // csv 引号字符，默认 '"'
  common::ObString escape_;     // csv 转义字符，默认 '"'
  common::ObString null_str_;   // 显式 NULL 文本，默认空
  bool has_header_;
  bool ignore_errors_;
  int64_t sample_size_;         // 类型推断采样行数，0 表示全量
  int64_t file_size_;           // 绑定期文件签名（变化检测）
  int64_t file_mtime_;
  common::ObSEArray<FileTableColumn, 16> columns_;
  FileTableDefinition()
    : kind_(FILE_TABLE_CSV), path_(), delimiter_(), quote_(), escape_(), null_str_(),
      has_header_(true), ignore_errors_(false), sample_size_(0), file_size_(-1),
      file_mtime_(-1), columns_()
  {}
};

// 解析表函数实参：第一个位置参数是文件路径，其余为 `name => value` 形式的命名参数。
int parse_file_table_arguments(const int32_t kind,
                               const _ParseNode *arguments,
                               common::ObIAllocator &allocator,
                               FileTableDefinition &definition,
                               const _ParseNode *&path_node,
                               int64_t &positional_count,
                               int64_t &argument_count);

int file_table_copy_string(common::ObIAllocator &allocator, const char *ptr,
                           const int64_t length, common::ObString &out);

// 读取文件头/采样数据行，推断列名、列类型与可空性。
int infer_file_table_columns(FileTableDefinition &definition,
                             common::ObIAllocator &allocator);

// 把绑定结果固化进执行计划，避免执行期重新推断导致结构漂移。
int compile_file_table_options(const TableItem &table,
                               common::ObIAllocator &allocator,
                               common::ObString &options);

int deserialize_file_table_definition(const common::ObString &options,
                                      common::ObIAllocator &allocator,
                                      FileTableDefinition &definition);

int create_file_table_scan(const TableFunctionScanContext &context,
                           ITableFunctionScan *&scan);

// 按行产出数据文件的读取器，行内顺序与 FileTableDefinition::columns_ 一致。
class FileTableRowSource
{
public:
  virtual ~FileTableRowSource() = default;
  virtual int open() = 0;
  virtual int next_row(common::ObIArray<common::ObObj> &values) = 0;
  virtual int close() = 0;
};

// 读取文件签名（大小 + 修改时间），供绑定期记录与执行期变化检测。
int file_table_stat(const common::ObString &path, int64_t &size, int64_t &mtime);

int create_file_table_row_source(const FileTableDefinition &definition,
                                 common::ObIAllocator &allocator,
                                 FileTableRowSource *&source);

// Parquet 专用（Footer Schema + Row Group 解码）。
int infer_parquet_columns(FileTableDefinition &definition, common::ObIAllocator &allocator);
int create_parquet_row_source(const FileTableDefinition &definition,
                              common::ObIAllocator &allocator,
                              FileTableRowSource *&source);

// 安全开关：SEEKDB_ENABLE_READ_FILES=0/off 关闭整个 read_* 家族；
// SEEKDB_READ_FILES_ROOT 指定允许访问的目录白名单（多个目录用 ';' 或 ':' 分隔）。
bool file_table_functions_enabled();
int check_file_table_path(const common::ObString &path);

// 供绑定与执行阶段共用的类型推导工具。
common::ObObjType widen_file_table_type(const common::ObObjType left, const common::ObObjType right);
// 从文本推断单个值的类型，并对超长/非法数值降级到 VARCHAR。
common::ObObjType detect_file_value_type(const common::ObString &text, bool quoted, bool *is_null);

} // namespace sql
} // namespace oceanbase
