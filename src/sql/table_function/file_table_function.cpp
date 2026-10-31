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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/table_function/file_table_function.h"
#include "sql/table_function/table_function.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/parser/parse_node.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/utility/ob_print_utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>

namespace oceanbase {
namespace sql {
using namespace common;

// 使用具名内部命名空间：sql 采用 unity build，多个 .cpp 会拼进同一个编译单元，
// 匿名命名空间会被合并，容易造成符号重定义。
namespace file_table_impl {

// 结果长度上限，避免恶意文件把 NULL/超长字段撑爆执行期内存。
const int64_t FILE_TABLE_MAX_VARCHAR_LENGTH = 65535;
const int64_t FILE_TABLE_MAX_FILE_SIZE = 512LL * 1024 * 1024;
const int64_t FILE_TABLE_MAX_COLUMNS = 1024;

struct FileBuffer
{
  FileBuffer(ObIAllocator &allocator) : allocator_(allocator), ptr_(nullptr), pos_(0), cap_(0) {}
  int append(const void *data, const int64_t length)
  {
    int ret = OB_SUCCESS;
    if (length < 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (pos_ + length > cap_) {
      int64_t new_cap = cap_ <= 0 ? 64 : cap_;
      while (new_cap < pos_ + length) { new_cap *= 2; }
      char *next = static_cast<char *>(allocator_.alloc(new_cap));
      if (next == nullptr) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (pos_ > 0) { MEMCPY(next, ptr_, pos_); }
        ptr_ = next;
        cap_ = new_cap;
      }
    }
    if (OB_SUCC(ret) && length > 0) {
      MEMCPY(ptr_ + pos_, data, length);
      pos_ += length;
    }
    return ret;
  }
  int append_i64(const int64_t value)
  {
    return append(&value, static_cast<int64_t>(sizeof(value)));
  }
  int append_i32(const int32_t value)
  {
    return append(&value, static_cast<int64_t>(sizeof(value)));
  }
  int append_bool(const bool value)
  {
    int8_t flag = value ? 1 : 0;
    return append(&flag, static_cast<int64_t>(sizeof(flag)));
  }
  int append_str(const ObString &str)
  {
    int ret = append_i64(str.length());
    if (OB_SUCC(ret) && str.length() > 0) { ret = append(str.ptr(), str.length()); }
    return ret;
  }
  ObIAllocator &allocator_;
  char *ptr_;
  int64_t pos_;
  int64_t cap_;
};

struct FileBufferReader
{
  explicit FileBufferReader(const ObString &buffer) : ptr_(buffer.ptr()), length_(buffer.length()), pos_(0) {}
  int read_i64(int64_t &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ + static_cast<int64_t>(sizeof(int64_t)) > length_) {
      ret = OB_INVALID_DATA;
    } else {
      MEMCPY(&value, ptr_ + pos_, sizeof(int64_t));
      pos_ += static_cast<int64_t>(sizeof(int64_t));
    }
    return ret;
  }
  int read_i32(int32_t &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ + static_cast<int64_t>(sizeof(int32_t)) > length_) {
      ret = OB_INVALID_DATA;
    } else {
      MEMCPY(&value, ptr_ + pos_, sizeof(int32_t));
      pos_ += static_cast<int64_t>(sizeof(int32_t));
    }
    return ret;
  }
  int read_bool(bool &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ + static_cast<int64_t>(sizeof(int8_t)) > length_) {
      ret = OB_INVALID_DATA;
    } else {
      int8_t flag = 0;
      MEMCPY(&flag, ptr_ + pos_, sizeof(int8_t));
      pos_ += static_cast<int64_t>(sizeof(int8_t));
      value = (flag != 0);
    }
    return ret;
  }
  int read_str(ObIAllocator &allocator, ObString &value)
  {
    int ret = OB_SUCCESS;
    int64_t length = 0;
    if (OB_FAIL(read_i64(length))) {
    } else if (length < 0 || pos_ + length > length_) {
      ret = OB_INVALID_DATA;
    } else if (length == 0) {
      value.reset();
    } else {
      char *copy = static_cast<char *>(allocator.alloc(length));
      if (copy == nullptr) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(copy, ptr_ + pos_, length);
        value.assign_ptr(copy, static_cast<int32_t>(length));
        pos_ += length;
      }
    }
    return ret;
  }
  const char *ptr_;
  int64_t length_;
  int64_t pos_;
};

bool parse_bool_text(const ObString &text, bool &value)
{
  bool ok = true;
  if (ObCharset::case_insensitive_equal(text, ObString::make_string("true")) ||
      ObCharset::case_insensitive_equal(text, ObString::make_string("on")) ||
      ObCharset::case_insensitive_equal(text, ObString::make_string("yes"))) {
    value = true;
  } else if (ObCharset::case_insensitive_equal(text, ObString::make_string("false")) ||
             ObCharset::case_insensitive_equal(text, ObString::make_string("off")) ||
             ObCharset::case_insensitive_equal(text, ObString::make_string("no"))) {
    value = false;
  } else {
    // 数字写法（"1"/"0"）按 0 与非 0 解释。
    int64_t begin = 0;
    while (begin < text.length() && (text.ptr()[begin] == '-' || text.ptr()[begin] == '+' ||
                                     (text.ptr()[begin] >= '0' && text.ptr()[begin] <= '9'))) {
      ++begin;
    }
    if (begin > 0 && begin == text.length()) {
      value = (0 != ::atoll(text.ptr()));
    } else {
      ok = false;
    }
  }
  return ok;
}

// 字面量节点取文本：快速参数化会把字面量换成 T_QUESTIONMARK，原始文本仍保留在
// str_value_（字符串）或 raw_text_（数字/布尔）里。
bool node_literal_text(const ParseNode *node, ObString &text)
{
  bool ok = false;
  if (nullptr != node) {
    if (nullptr != node->str_value_ && node->str_len_ > 0) {
      text.assign_ptr(node->str_value_, static_cast<int32_t>(node->str_len_));
      ok = true;
    } else if (nullptr != node->raw_text_ && node->text_len_ > 0) {
      text.assign_ptr(node->raw_text_, static_cast<int32_t>(node->text_len_));
      ok = true;
    }
  }
  return ok;
}

bool node_is_named_argument(const ParseNode *node)
{
  // 命名参数在语法上是 "名字 = 值"（含 "名字 => 值"），即 T_OP_EQ 且有两个孩子。
  // 名字节点可能是标识符，也可能是被词法器标记为关键字，其文本字段不一定落在 str_value_。
  return node != nullptr && node->type_ == T_OP_EQ && node->num_child_ == 2 &&
         node->children_ != nullptr && node->children_[0] != nullptr &&
         node->children_[1] != nullptr;
}

// 取出命名参数的名字文本；优先 str_value_，其次回退到原始文本。
bool named_argument_name(const ParseNode *node, ObString &name)
{
  bool ok = false;
  if (node != nullptr && node->children_ != nullptr && node->children_[0] != nullptr) {
    const ParseNode *name_node = node->children_[0];
    if (nullptr != name_node->str_value_ && name_node->str_len_ > 0) {
      name.assign_ptr(name_node->str_value_, static_cast<int32_t>(name_node->str_len_));
      ok = true;
    } else if (nullptr != name_node->raw_text_ && name_node->text_len_ > 0) {
      name.assign_ptr(name_node->raw_text_, static_cast<int32_t>(name_node->text_len_));
      ok = true;
    }
  }
  return ok;
}

} // namespace file_table_impl

using namespace file_table_impl;

int file_table_copy_string(ObIAllocator &allocator, const char *ptr, const int64_t length, ObString &out)
{
  int ret = OB_SUCCESS;
  if (length <= 0) {
    out.reset();
  } else {
    char *copy = static_cast<char *>(allocator.alloc(length));
    if (copy == nullptr) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(copy, ptr, length);
      out.assign_ptr(copy, static_cast<int32_t>(length));
    }
  }
  return ret;
}

namespace file_table_option_impl {

int literal_string(const ParseNode *node, ObString &value)
{
  int ret = OB_SUCCESS;
  if (node == nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else if (node->type_ == T_BOOL) {
    value = ObString::make_string(node->value_ == 0 ? "false" : "true");
  } else if (node_literal_text(node, value)) {
    // 字符串、被快速参数化替换后的 T_QUESTIONMARK、以及数字字面量都能取到原始文本。
  } else if (0 == node->num_child_ && nullptr != node->str_value_ &&
             T_NUMBER != node->type_ && T_BOOL != node->type_) {
    // 其它字面量节点（如字符串前缀/字符集变体）统一按文本处理。
    value.assign_ptr(node->str_value_, static_cast<int32_t>(node->str_len_));
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

int option_bool(const ParseNode *node, bool &value)
{
  int ret = OB_SUCCESS;
  if (node == nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else if (node->type_ == T_BOOL) {
    value = (node->value_ != 0);
  } else if (node->type_ == T_NUMBER) {
    value = (node->value_ != 0);
  } else {
    ObString text;
    if (!node_literal_text(node, text) || !parse_bool_text(text, value)) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int option_int(const ParseNode *node, int64_t &value)
{
  int ret = OB_SUCCESS;
  if (node == nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else if (node->type_ == T_NUMBER) {
    value = node->value_;
  } else {
    ObString text;
    if (!node_literal_text(node, text)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      value = ::atoll(text.ptr());
    }
  }
  return ret;
}

int apply_named_option(FileTableDefinition &definition,
                       const ObString &name,
                       const ParseNode *value_node,
                       ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  bool flag = false;
  int64_t number = 0;
  ObString text;
  if (ObCharset::case_insensitive_equal(name, ObString::make_string("header"))) {
    OZ (option_bool(value_node, flag));
    definition.has_header_ = flag;
  } else if (ObCharset::case_insensitive_equal(name, ObString::make_string("delim") ) ||
             ObCharset::case_insensitive_equal(name, ObString::make_string("delimiter")) ||
             ObCharset::case_insensitive_equal(name, ObString::make_string("sep"))) {
    OZ (literal_string(value_node, text));
    if (OB_SUCC(ret) && text.length() != 1) { ret = OB_INVALID_ARGUMENT; }
    OZ (file_table_copy_string(allocator, text.ptr(), text.length(), definition.delimiter_));
  } else if (ObCharset::case_insensitive_equal(name, ObString::make_string("quote"))) {
    OZ (literal_string(value_node, text));
    OZ (file_table_copy_string(allocator, text.ptr(), text.length(), definition.quote_));
  } else if (ObCharset::case_insensitive_equal(name, ObString::make_string("escape"))) {
    OZ (literal_string(value_node, text));
    OZ (file_table_copy_string(allocator, text.ptr(), text.length(), definition.escape_));
  } else if (ObCharset::case_insensitive_equal(name, ObString::make_string("nullstr")) ||
             ObCharset::case_insensitive_equal(name, ObString::make_string("null_str")) ||
             ObCharset::case_insensitive_equal(name, ObString::make_string("nullstrs"))) {
    OZ (literal_string(value_node, text));
    OZ (file_table_copy_string(allocator, text.ptr(), text.length(), definition.null_str_));
  } else if (ObCharset::case_insensitive_equal(name, ObString::make_string("ignore_errors")) ||
             ObCharset::case_insensitive_equal(name, ObString::make_string("ignoreerrors"))) {
    OZ (option_bool(value_node, flag));
    definition.ignore_errors_ = flag;
  } else if (ObCharset::case_insensitive_equal(name, ObString::make_string("sample_size")) ||
             ObCharset::case_insensitive_equal(name, ObString::make_string("sample"))) {
    OZ (option_int(value_node, number));
    // 只支持“全量推断”：非负采样上限会改变推断结果，明确报不支持；
    // -1 与 0 等价，都表示扫描全部数据推断列类型。
    if (OB_SUCC(ret) && number > 0) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size");
    } else if (OB_SUCC(ret)) {
      definition.sample_size_ = 0;
    }
  } else if (ObCharset::case_insensitive_equal(name, ObString::make_string("format"))) {
    // DuckDB 兼容写法：CSV 支持 csv/auto，JSON 支持 newline_delimited/json。
    OZ (literal_string(value_node, text));
    if (OB_SUCC(ret) &&
        !ObCharset::case_insensitive_equal(text, ObString::make_string("csv")) &&
        !ObCharset::case_insensitive_equal(text, ObString::make_string("auto")) &&
        !ObCharset::case_insensitive_equal(text, ObString::make_string("newline_delimited")) &&
        !ObCharset::case_insensitive_equal(text, ObString::make_string("json"))) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file format");
    }
  } else if (ObCharset::case_insensitive_equal(name, ObString::make_string("filename")) ||
             ObCharset::case_insensitive_equal(name, ObString::make_string("auto_detect")) ||
             ObCharset::case_insensitive_equal(name, ObString::make_string("columns"))) {
    // 真实路径始终由第一个位置参数给出；其余 DuckDB 兼容开关暂不改变行为。
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unknown read file option");
  }
  return ret;
}

} // namespace file_table_option_impl

using namespace file_table_option_impl;

bool file_table_functions_enabled()
{
  const char *value = ::getenv("SEEKDB_ENABLE_READ_FILES");
  if (value == nullptr || value[0] == '\0') { return true; }
  return !(value[0] == '0' || value[0] == 'o' || value[0] == 'O' || value[0] == 'n' || value[0] == 'N');
}

int check_file_table_path(const ObString &path)
{
  int ret = OB_SUCCESS;
  if (!file_table_functions_enabled()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "read_csv/read_json/read_parquet is disabled");
  } else if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
  }
  // 拒绝 ".." 路径段，避免越权访问。
  for (int64_t i = 0; OB_SUCC(ret) && i + 1 < path.length(); ++i) {
    if (path.ptr()[i] == '.' && path.ptr()[i + 1] == '.' &&
        (i == 0 || path.ptr()[i - 1] == '/' || path.ptr()[i - 1] == '\\') &&
        (i + 2 >= path.length() || path.ptr()[i + 2] == '/' || path.ptr()[i + 2] == '\\')) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "path must not contain '..'");
    }
  }
  const char *root = ::getenv("SEEKDB_READ_FILES_ROOT");
  if (OB_SUCC(ret) && root != nullptr && root[0] != '\0') {
    bool matched = false;
    ObString roots = ObString::make_string(root);
    int64_t start = 0;
    while (!matched && start <= roots.length()) {
      int64_t end = start;
      while (end < roots.length() && roots.ptr()[end] != ';' && roots.ptr()[end] != ':') { ++end; }
      ObString prefix(end - start, roots.ptr() + start);
      if (!prefix.empty() && prefix.length() <= path.length() &&
          0 == strncmp(prefix.ptr(), path.ptr(), prefix.length())) {
        matched = true;
      } else {
        start = end + 1;
      }
    }
    if (!matched) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "path is outside of the allowed root");
    }
  }
  return ret;
}

common::ObObjType widen_file_table_type(const ObObjType left, const ObObjType right)
{
  ObObjType result = ObVarcharType;
  if (left == ObNullType) {
    result = right;
  } else if (right == ObNullType) {
    result = left;
  } else if (left == right) {
    result = left;
  } else if ((left == ObIntType && right == ObDoubleType) ||
             (left == ObDoubleType && right == ObIntType)) {
    result = ObDoubleType;
  } else if ((left == ObIntType && right == ObTinyIntType) ||
             (left == ObTinyIntType && right == ObIntType) ||
             (left == ObTinyIntType && right == ObTinyIntType)) {
    result = ObIntType;
  } else if (left == ObNumberType || right == ObNumberType) {
    // NUMBER 优先级最高：整数与小数混排时统一按 NUMBER，且保留每个值自己的小数位数。
    // 与 DOUBLE 混排也收敛到 NUMBER，保证同一列内输出格式一致。
    if (left == ObNumberType && right == ObNullType) {
      result = ObNumberType;
    } else if (right == ObNumberType && left == ObNullType) {
      result = ObNumberType;
    } else if (left == ObVarcharType || right == ObVarcharType ||
               left == ObDateType || right == ObDateType ||
               left == ObDateTimeType || right == ObDateTimeType) {
      result = ObVarcharType;
    } else {
      result = ObNumberType;
    }
  } else if ((left == ObDateType && right == ObDateTimeType) ||
             (left == ObDateTimeType && right == ObDateType)) {
    result = ObDateTimeType;
  } else {
    result = ObVarcharType;
  }
  return result;
}

common::ObObjType detect_file_value_type(const ObString &text, bool quoted, bool *is_null)
{
  ObObjType type = ObVarcharType;
  bool null_value = false;
  if (text.empty() && !quoted) {
    null_value = true;
  } else if (text.length() >= 1 && text.ptr()[0] == '\0') {
    null_value = true;
  } else {
    // 类型推断时，bool 只接受字面量 true/false/on/off/yes/no；
    // 数字（包括 "1"/"0"）按数值处理，否则 "10" 会被误判为 bool(true) 进而在转换时变成 1。
    if (ObCharset::case_insensitive_equal(text, ObString::make_string("true")) ||
        ObCharset::case_insensitive_equal(text, ObString::make_string("false")) ||
        ObCharset::case_insensitive_equal(text, ObString::make_string("on")) ||
        ObCharset::case_insensitive_equal(text, ObString::make_string("off")) ||
        ObCharset::case_insensitive_equal(text, ObString::make_string("yes")) ||
        ObCharset::case_insensitive_equal(text, ObString::make_string("no"))) {
      type = ObTinyIntType;
    } else {
      // 数字：整数优先，超出 int64 范围退化为 DOUBLE，再不行就是 VARCHAR。
      const char *begin = text.ptr();
      const char *end = text.ptr() + text.length();
      bool is_number = true;      // 允许一个前导符号、数字、小数点与指数符号
      bool has_fraction = false;  // 出现过 '.' 或 'e'/'E'
      bool digits_seen = false;
      for (const char *p = begin; p < end && is_number; ++p) {
        if (*p == '+' || *p == '-') {
          if (p != begin) { is_number = false; }
        } else if (*p >= '0' && *p <= '9') {
          digits_seen = true;
        } else if (*p == '.' || *p == 'e' || *p == 'E') {
          has_fraction = true;
        } else {
          is_number = false;
        }
      }
      if (is_number && digits_seen) {
        if (!has_fraction) {
          errno = 0;
          char *stop = nullptr;
          (void) ::strtoll(begin, &stop, 10);
          if (stop == end && errno == 0) {
            type = ObIntType;
          }
        }
        if (type == ObVarcharType) {
          // 小数用 NUMBER：按文本保留原始小数位数，输出 98.5/87/90.25 而不是 98.5/87.0/90.25。
          errno = 0;
          char *stop = nullptr;
          (void) ::strtod(begin, &stop);
          if (stop == end && errno == 0) { type = ObNumberType; }
        }
      }
      if (type == ObVarcharType) {
        // 日期与时间：YYYY-MM-DD / YYYY-MM-DD HH:MM:SS
        bool date_like = (text.length() == 10);
        bool datetime_like = (text.length() >= 19);
        for (int64_t i = 0; i < text.length(); ++i) {
          char c = text.ptr()[i];
          bool ok = (c >= '0' && c <= '9') || c == '-' || c == ':' || c == ' ' || c == '.' || c == 'T';
          if (!ok) {
            date_like = false;
            datetime_like = false;
            break;
          }
        }
        if (date_like && text.ptr()[4] == '-' && text.ptr()[7] == '-') {
          type = ObDateType;
        } else if (datetime_like && text.ptr()[4] == '-' && text.ptr()[7] == '-' &&
                   (text.ptr()[10] == ' ' || text.ptr()[10] == 'T')) {
          type = ObDateTimeType;
        }
      }
    }
  }
  if (is_null != nullptr) { *is_null = null_value; }
  return type;
}

int parse_file_table_arguments(const int32_t kind,
                               const ParseNode *arguments,
                               ObIAllocator &allocator,
                               FileTableDefinition &definition,
                               const ParseNode *&path_node,
                               int64_t &positional_count,
                               int64_t &argument_count)
{
  int ret = OB_SUCCESS;
  path_node = nullptr;
  positional_count = 0;
  argument_count = 0;
  definition.kind_ = static_cast<FileTableKind>(kind);
  definition.delimiter_ = ObString::make_string(",");
  definition.quote_ = ObString::make_string("\"");
  definition.escape_ = ObString::make_string("\"");
  if (arguments != nullptr) {
    // 语法层的 expr_list 是 T_EXPR_LIST 扁平数组（num_child_ 为参数个数，children_ 为各参数），
    // 见 sql_parser_mysql_mode.y 的 expr_list 规则；这里按扁平数组逐个处理。
    if (T_EXPR_LIST == arguments->type_ && nullptr != arguments->children_) {
      argument_count = arguments->num_child_;
      for (int64_t i = 0; OB_SUCC(ret) && i < arguments->num_child_; ++i) {
        const ParseNode *child = arguments->children_[i];
        if (OB_ISNULL(child)) {
          continue;
        }
        if (node_is_named_argument(child)) {
          ObString name;
          if (!named_argument_name(child, name)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "invalid named argument name");
          } else {
            OZ (apply_named_option(definition, name, child->children_[1], allocator));
          }
        } else if (positional_count == 0) {
          path_node = child;
          positional_count = 1;
        } else {
          // 文件路径只允许一个位置参数；第二个位置参数（如 DuckDB 的格式字符串）语法非法。
          ret = OB_ERR_PARSE_SQL;
        }
      }
    } else {
      // 兜底：未经 expr_list 包装的单个参数节点。
      argument_count = 1;
      if (node_is_named_argument(arguments)) {
        ObString name;
        if (!named_argument_name(arguments, name)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "invalid named argument name");
        } else {
          OZ (apply_named_option(definition, name, arguments->children_[1], allocator));
        }
      } else {
        path_node = arguments;
        positional_count = 1;
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObString path;
    if (path_node == nullptr) {
      ret = OB_ERR_PARAM_SIZE;
      LOG_USER_ERROR(OB_ERR_PARAM_SIZE, static_cast<int>(strlen("read file")), "read file");
    } else if (OB_FAIL(literal_string(path_node, path))) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path must be a string literal");
    } else if (OB_FAIL(file_table_copy_string(allocator, path.ptr(), path.length(), definition.path_))) {
    } else if (OB_FAIL(check_file_table_path(definition.path_))) {
    }
  }
  return ret;
}

int compile_file_table_options(const TableItem &table, ObIAllocator &allocator, ObString &options)
{
  int ret = OB_SUCCESS;
  FileTableDefinition *definition = table.file_table_def_;
  if (definition == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    FileBuffer buffer(allocator);
    OZ (buffer.append_i32(1));  // 版本号
    OZ (buffer.append_i32(static_cast<int32_t>(definition->kind_)));
    OZ (buffer.append_bool(definition->has_header_));
    OZ (buffer.append_bool(definition->ignore_errors_));
    OZ (buffer.append_i64(definition->sample_size_));
    OZ (buffer.append_i64(definition->file_size_));
    OZ (buffer.append_i64(definition->file_mtime_));
    OZ (buffer.append_str(definition->path_));
    OZ (buffer.append_str(definition->delimiter_));
    OZ (buffer.append_str(definition->quote_));
    OZ (buffer.append_str(definition->escape_));
    OZ (buffer.append_str(definition->null_str_));
    OZ (buffer.append_i64(definition->columns_.count()));
    for (int64_t i = 0; OB_SUCC(ret) && i < definition->columns_.count(); ++i) {
      const FileTableColumn &column = definition->columns_.at(i);
      OZ (buffer.append_str(column.name_));
      OZ (buffer.append_i32(static_cast<int32_t>(column.type_)));
      OZ (buffer.append_i32(column.length_));
      OZ (buffer.append_bool(column.nullable_));
    }
    if (OB_SUCC(ret)) {
      options.assign_ptr(buffer.ptr_, static_cast<int32_t>(buffer.pos_));
    }
  }
  return ret;
}

int deserialize_file_table_definition(const ObString &options,
                                      ObIAllocator &allocator,
                                      FileTableDefinition &definition)
{
  int ret = OB_SUCCESS;
  FileBufferReader reader(options);
  int32_t version = 0;
  int32_t kind = 0;
  int64_t count = 0;
  OZ (reader.read_i32(version));
  if (OB_SUCC(ret) && version != 1) { ret = OB_INVALID_DATA; }
  OZ (reader.read_i32(kind));
  if (OB_SUCC(ret) && (kind < FILE_TABLE_CSV || kind > FILE_TABLE_GLOB)) { ret = OB_INVALID_DATA; }
  if (OB_SUCC(ret)) { definition.kind_ = static_cast<FileTableKind>(kind); }
  OZ (reader.read_bool(definition.has_header_));
  OZ (reader.read_bool(definition.ignore_errors_));
  OZ (reader.read_i64(definition.sample_size_));
  OZ (reader.read_i64(definition.file_size_));
  OZ (reader.read_i64(definition.file_mtime_));
  OZ (reader.read_str(allocator, definition.path_));
  OZ (reader.read_str(allocator, definition.delimiter_));
  OZ (reader.read_str(allocator, definition.quote_));
  OZ (reader.read_str(allocator, definition.escape_));
  OZ (reader.read_str(allocator, definition.null_str_));
  OZ (reader.read_i64(count));
  if (OB_SUCC(ret) && (count < 0 || count > FILE_TABLE_MAX_COLUMNS)) { ret = OB_INVALID_DATA; }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    FileTableColumn column;
    int32_t type = 0;
    OZ (reader.read_str(allocator, column.name_));
    OZ (reader.read_i32(type));
    OZ (reader.read_i32(column.length_));
    OZ (reader.read_bool(column.nullable_));
    if (OB_SUCC(ret)) {
      column.type_ = static_cast<ObObjType>(type);
      OZ (definition.columns_.push_back(column));
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

