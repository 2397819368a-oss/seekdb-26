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
#include "sql/table_function/file_table_function.h"
#include "sql/table_function/table_function.h"
#include "lib/utility/ob_print_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <dirent.h>
#endif

namespace oceanbase {
namespace sql {
using namespace common;

// 具名内部命名空间，避免 unity build 下与其他 .cpp 合并后符号冲突。
namespace file_table_reader_impl {

const int64_t FILE_TABLE_MAX_FILE_SIZE = 512LL * 1024 * 1024;
const int64_t FILE_TABLE_MAX_FIELDS = 4096;
const int64_t FILE_TABLE_MAX_VARCHAR_LENGTH = 65535;

struct FileStat
{
  int64_t size_;
  int64_t mtime_;
  FileStat() : size_(-1), mtime_(-1) {}
};

int stat_file(const ObString &path, FileStat &info)
{
  int ret = OB_SUCCESS;
  char buffer[1024];
  if (path.length() <= 0 || path.length() >= static_cast<int32_t>(sizeof(buffer) - 1)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(buffer, path.ptr(), path.length());
    buffer[path.length()] = '\0';
    struct stat st;
    if (0 != ::stat(buffer, &st)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("file not exist", K(path), K(errno));
    } else if (static_cast<int64_t>(st.st_size) > FILE_TABLE_MAX_FILE_SIZE) {
      ret = OB_SIZE_OVERFLOW;
      LOG_USER_ERROR(OB_SIZE_OVERFLOW, path.length(), path.ptr());
    } else {
      info.size_ = static_cast<int64_t>(st.st_size);
      info.mtime_ = static_cast<int64_t>(st.st_mtime);
    }
  }
  return ret;
}

int read_file(const ObString &path, ObIAllocator &allocator, char *&data, int64_t &length)
{
  int ret = OB_SUCCESS;
  data = nullptr;
  length = 0;
  FileStat info;
  FILE *file = nullptr;
  char name[1024];
  if (path.length() <= 0 || path.length() >= static_cast<int32_t>(sizeof(name) - 1)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(name, path.ptr(), path.length());
    name[path.length()] = '\0';
    if (OB_FAIL(stat_file(path, info))) {
    } else if ((file = ::fopen(name, "rb")) == nullptr) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("open file failed", K(path), K(errno));
    } else {
      char *buffer = static_cast<char *>(allocator.alloc(info.size_ + 1));
      if (buffer == nullptr) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (info.size_ > 0 &&
                 static_cast<int64_t>(::fread(buffer, 1, info.size_, file)) != info.size_) {
        ret = OB_IO_ERROR;
        LOG_WARN("read file failed", K(path));
      } else {
        buffer[info.size_] = '\0';
        data = buffer;
        length = info.size_;
      }
      (void) ::fclose(file);
    }
  }
  return ret;
}

bool has_glob_magic(const ObString &path)
{
  bool found = false;
  for (int64_t i = 0; !found && i < path.length(); ++i) {
    const char c = path.ptr()[i];
    found = (c == '*' || c == '?' || c == '[');
  }
  return found;
}

// 极简 glob 匹配：'*' 匹配任意串，'?' 匹配单字符。
bool glob_match(const char *pattern, const int64_t pattern_length, const char *text,
                const int64_t text_length)
{
  int64_t p = 0;
  int64_t t = 0;
  int64_t star = -1;
  int64_t mark = 0;
  while (t < text_length) {
    if (p < pattern_length && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern_length && pattern[p] == '*') {
      star = p++;
      mark = t;
    } else if (star >= 0) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern_length && pattern[p] == '*') { ++p; }
  return p == pattern_length;
}

int expand_glob(const ObString &pattern, ObIAllocator &allocator, ObIArray<ObString> &paths)
{
  int ret = OB_SUCCESS;
  int64_t slash = -1;
  for (int64_t i = pattern.length() - 1; i >= 0 && slash < 0; --i) {
    if (pattern.ptr()[i] == '/' || pattern.ptr()[i] == '\\') { slash = i; }
  }
  const ObString dir = slash <= 0 ? ObString(static_cast<int32_t>(slash < 0 ? 0 : 1), pattern.ptr()) :
                         ObString(static_cast<int32_t>(slash), pattern.ptr());
  const ObString name_pattern(static_cast<int32_t>(pattern.length() - slash - 1),
                              pattern.ptr() + slash + 1);
  ObSEArray<ObString, 16> names;
  ObString search_dir = dir.empty() ? ObString::make_string(".") : dir;
#if defined(_WIN32)
  char dir_name[1024];
  if (search_dir.length() >= static_cast<int32_t>(sizeof(dir_name) - 4)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // Windows 的目录枚举接口对混合分隔符不可靠（SQL 里习惯写正斜杠），
    // 这里先把目录统一转成反斜杠再拼通配符。
    int64_t pos = 0;
    for (int64_t i = 0; i < search_dir.length(); ++i) {
      dir_name[pos++] = (search_dir.ptr()[i] == '/') ? '\\' : search_dir.ptr()[i];
    }
    const char *suffix = (pos == 1 && dir_name[0] == '\\') ? "*" : "\\*";
    const int64_t suffix_length = static_cast<int64_t>(strlen(suffix));
    MEMCPY(dir_name + pos, suffix, suffix_length);
    dir_name[pos + suffix_length] = '\0';
    struct _finddata_t entry;
    intptr_t handle = ::_findfirst(dir_name, &entry);
    if (handle == -1) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("glob expand failed to open directory", K(search_dir), K(dir_name));
    } else {
      do {
        if (!(entry.attrib & _A_SUBDIR)) {
          const int64_t length = static_cast<int64_t>(strlen(entry.name));
          if (glob_match(name_pattern.ptr(), name_pattern.length(), entry.name, length)) {
            ObString file_name;
            if (OB_FAIL(file_table_copy_string(allocator, entry.name, length, file_name))) {
            } else if (OB_FAIL(names.push_back(file_name))) {
            } else {
            }
          }
        }
      } while (OB_SUCC(ret) && 0 == ::_findnext(handle, &entry));
      ::_findclose(handle);
    }
  }
#else
  // ObString 不保证以 '\0' 结尾，传给 opendir 前必须先拷贝到本地缓冲区。
  char dir_path[1024];
  DIR *dir_handle = nullptr;
  if (search_dir.length() <= 0 ||
      search_dir.length() >= static_cast<int32_t>(sizeof(dir_path) - 1)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(dir_path, search_dir.ptr(), search_dir.length());
    dir_path[search_dir.length()] = '\0';
    dir_handle = ::opendir(dir_path);
  }
  if (OB_FAIL(ret)) {
  } else if (dir_handle == nullptr) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    struct dirent *entry = nullptr;
    while (OB_SUCC(ret) && (entry = ::readdir(dir_handle)) != nullptr) {
      const int64_t length = static_cast<int64_t>(strlen(entry->d_name));
      if (entry->d_name[0] != '.' &&
          glob_match(name_pattern.ptr(), name_pattern.length(), entry->d_name, length)) {
        ObString file_name;
        if (OB_FAIL(file_table_copy_string(allocator, entry->d_name, length, file_name))) {
        } else if (OB_FAIL(names.push_back(file_name))) {
        }
      }
    }
    ::closedir(dir_handle);
  }
#endif
  // 排序后拼接完整路径，保证多次执行结果顺序稳定。
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    for (int64_t j = i + 1; j < names.count(); ++j) {
      // ObString 不以 '\0' 结尾，不能直接用 strcmp，按长度前缀比较。
      const ObString &left = names.at(i);
      const ObString &right = names.at(j);
      const int32_t min_length = left.length() < right.length() ? left.length() : right.length();
      int cmp = 0;
      if (min_length > 0) {
        cmp = memcmp(left.ptr(), right.ptr(), min_length);
      }
      if (0 == cmp) {
        cmp = left.length() - right.length();
      }
      if (0 < cmp) {
        const ObString tmp = names.at(i);
        names.at(i) = names.at(j);
        names.at(j) = tmp;
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    const ObString &file_name = names.at(i);
    const int64_t total = dir.length() + 1 + file_name.length();
    char *buffer = static_cast<char *>(allocator.alloc(total));
    if (buffer == nullptr) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t pos = 0;
      MEMCPY(buffer, dir.ptr(), dir.length());
      pos += dir.length();
      if (pos > 0) {
        if (buffer[pos - 1] == '/' || buffer[pos - 1] == '\\') { --pos; }
        buffer[pos++] = '/';
      }
      MEMCPY(buffer + pos, file_name.ptr(), file_name.length());
      pos += file_name.length();
      OZ (paths.push_back(ObString(static_cast<int32_t>(pos), buffer)));
    }
  }
  return ret;
}

int resolve_files(const ObString &path, ObIAllocator &allocator, ObIArray<ObString> &paths)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_file_table_path(path))) {
  } else if (has_glob_magic(path)) {
    OZ (expand_glob(path, allocator, paths));
    if (OB_SUCC(ret) && paths.empty()) {
      ret = OB_FILE_NOT_EXIST;
      LOG_USER_ERROR(OB_FILE_NOT_EXIST, path.length(), path.ptr());
    }
  } else {
    FileStat info;
    OZ (stat_file(path, info));
    OZ (paths.push_back(path));
  }
  return ret;
}

int32_t civil_to_days(const int32_t year, const int32_t month, const int32_t day)
{
  const int32_t y = year - (month <= 2);
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const int32_t yoe = y - era * 400;
  const int32_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

bool parse_date_text(const ObString &text, int32_t &days)
{
  int64_t ok = text.length() >= 10 && text.ptr()[4] == '-' && text.ptr()[7] == '-';
  for (int64_t i = 0; ok && i < 10; ++i) {
    const char c = text.ptr()[i];
    if (i != 4 && i != 7) { ok = (c >= '0' && c <= '9'); }
  }
  if (ok) {
    const int32_t year = ::atoi(text.ptr());
    const int32_t month = ::atoi(text.ptr() + 5);
    const int32_t day = ::atoi(text.ptr() + 8);
    ok = year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
    if (ok) { days = civil_to_days(year, month, day); }
  }
  return ok;
}

// 返回微秒数（ObObj 里 datetime 以微秒存储），支持可选的 ".ffffff" 小数秒。
bool parse_datetime_text(const ObString &text, int64_t &microseconds)
{
  int32_t days = 0;
  bool ok = text.length() >= 19 && parse_date_text(ObString(10, text.ptr()), days) &&
            (text.ptr()[10] == ' ' || text.ptr()[10] == 'T') && text.ptr()[13] == ':' &&
            text.ptr()[16] == ':';
  int64_t fraction = 0;
  int64_t digits = 0;
  if (ok && text.length() > 19) {
    ok = text.ptr()[19] == '.';
    for (int64_t i = 20; ok && digits < 6 && i < text.length(); ++i, ++digits) {
      const char c = text.ptr()[i];
      if (c < '0' || c > '9') {
        ok = false;
      } else {
        fraction = fraction * 10 + (c - '0');
      }
    }
    ok = ok && 20 + digits == static_cast<int64_t>(text.length());
    while (digits < 6) {
      fraction *= 10;
      ++digits;
    }
  }
  if (ok) {
    const int32_t hour = ::atoi(text.ptr() + 11);
    const int32_t minute = ::atoi(text.ptr() + 14);
    const int32_t second = ::atoi(text.ptr() + 17);
    ok = hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 && second < 61;
    if (ok) {
      const int64_t seconds =
          static_cast<int64_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
      microseconds = seconds * 1000000LL + fraction;
    }
  }
  return ok;
}

int make_varchar(const ObString &text, ObIAllocator &allocator, ObObj &obj)
{
  int ret = OB_SUCCESS;
  ObString copy;
  if (OB_FAIL(file_table_copy_string(allocator, text.ptr(), text.length(), copy))) {
  } else {
    obj.set_varchar(copy);
    obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  }
  return ret;
}

int convert_text_to_obj(const ObObjType type, const ObString &text, ObIAllocator &allocator, ObObj &obj)
{
  int ret = OB_SUCCESS;
  if (text.empty()) {
    obj.set_null();
  } else {
    switch (type) {
      case ObIntType: {
        errno = 0;
        char *stop = nullptr;
        const int64_t value = ::strtoll(text.ptr(), &stop, 10);
        if (stop != text.ptr() + text.length() || 0 != errno) {
          ret = OB_INVALID_DATA;
        } else {
          obj.set_int(value);
        }
        break;
      }
      case ObDoubleType: {
        errno = 0;
        char *stop = nullptr;
        const double value = ::strtod(text.ptr(), &stop);
        if (stop != text.ptr() + text.length() || 0 != errno) {
          ret = OB_INVALID_DATA;
        } else {
          obj.set_double(value);
        }
        break;
      }
      case ObNumberType: {
        // 按文本原样构造 NUMBER，保留字面量自带的小数位数（98.5 保持 1 位、87 保持 0 位），
        // 不能走 double 中转，否则会丢精度或多出 ".0"。
        number::ObNumber value;
        if (OB_FAIL(value.from(text.ptr(), static_cast<int64_t>(text.length()), allocator))) {
          ret = OB_INVALID_DATA;
        } else {
          obj.set_number(value);
        }
        break;
      }
      case ObTinyIntType: {
        bool value = false;
        if (!file_table_impl::parse_bool_text(text, value)) {
          ret = OB_INVALID_DATA;
        } else {
          obj.set_tinyint(value ? 1 : 0);
        }
        break;
      }
      case ObDateType: {
        int32_t days = 0;
        if (!parse_date_text(text, days)) {
          ret = OB_INVALID_DATE_VALUE;
        } else {
          obj.set_date(days);
        }
        break;
      }
      case ObDateTimeType: {
        int64_t microseconds = 0;
        if (!parse_datetime_text(text, microseconds)) {
          ret = OB_INVALID_DATE_VALUE;
        } else {
          obj.set_datetime(microseconds);
        }
        break;
      }
      default:
        OZ (make_varchar(text, allocator, obj));
        break;
    }
  }
  return ret;
}

struct RawField
{
  ObString text_;
  bool quoted_;
  RawField() : text_(), quoted_(false) {}
};

// CSV 记录解析：引号包裹、"" 与 escape 转义、字段内换行、LF/CRLF、UTF-8 BOM。
class CsvParser
{
public:
  CsvParser(const char *data, const int64_t length, const ObString &delimiter,
            const ObString &quote, const ObString &escape)
    : data_(data), length_(length), pos_(0), line_(1), delimiter_(delimiter), quote_(quote),
      escape_(escape)
  {
    if (length_ >= 3 && data_[0] == '\xEF' && data_[1] == '\xBB' && data_[2] == '\xBF') {
      pos_ = 3;
    }
  }
  int next_record(ObIAllocator &allocator, ObIArray<RawField> &fields, int64_t &line)
  {
    int ret = OB_SUCCESS;
    fields.reuse();
    line = line_;
    if (pos_ >= length_) {
      ret = OB_ITER_END;
    }
    while (OB_SUCC(ret) && pos_ < length_) {
      RawField field;
      if (is_quote(data_[pos_])) {
        field.quoted_ = true;
        ++pos_;
        char *buffer = static_cast<char *>(allocator.alloc(length_ - pos_ + 1));
        int64_t write = 0;
        bool closed = false;
        if (buffer == nullptr) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        }
        while (OB_SUCC(ret) && pos_ < length_ && !closed) {
          const char c = data_[pos_];
          if (c == '\\' && pos_ + 1 < length_ && is_escape_backslash()) {
            buffer[write++] = data_[pos_ + 1];
            pos_ += 2;
          } else if (is_quote(c)) {
            if (pos_ + 1 < length_ && is_quote(data_[pos_ + 1])) {
              buffer[write++] = c;
              pos_ += 2;
            } else {
              ++pos_;
              closed = true;
            }
          } else {
            if (c == '\r') {
              // 引号内的 CRLF 统一归一化成 LF，否则 CR 会混进字段值里。
              if (pos_ + 1 < length_ && data_[pos_ + 1] == '\n') { ++pos_; }
              buffer[write++] = '\n';
              ++line_;
              ++pos_;
            } else {
              if (c == '\n') { ++line_; }
              buffer[write++] = c;
              ++pos_;
            }
          }
        }
        if (OB_SUCC(ret)) {
          if (!closed) {
            ret = OB_INVALID_DATA;
            LOG_WARN("unterminated quoted field in CSV", K(line));
          } else {
            field.text_.assign_ptr(buffer, static_cast<int32_t>(write));
          }
        }
      } else {
        const int64_t start = pos_;
        while (pos_ < length_ && !is_delimiter(data_[pos_]) &&
               data_[pos_] != '\n' && data_[pos_] != '\r') {
          ++pos_;
        }
        field.text_.assign_ptr(data_ + start, static_cast<int32_t>(pos_ - start));
      }
      if (OB_SUCC(ret)) {
        OZ (fields.push_back(field));
        if (OB_SUCC(ret) && fields.count() > FILE_TABLE_MAX_FIELDS) {
          ret = OB_SIZE_OVERFLOW;
        }
        if (OB_SUCC(ret)) {
          if (pos_ < length_ && is_delimiter(data_[pos_])) {
            ++pos_;
            continue;
          }
          // 记录结束
          if (pos_ < length_ && data_[pos_] == '\r') { ++pos_; }
          if (pos_ < length_ && data_[pos_] == '\n') { ++pos_; }
          ++line_;
          break;
        }
      }
    }
    return ret;
  }
private:
  bool is_quote(const char c) const { return quote_.length() == 1 && c == quote_.ptr()[0]; }
  bool is_delimiter(const char c) const { return delimiter_.length() == 1 && c == delimiter_.ptr()[0]; }
  bool is_escape_backslash() const { return escape_.length() == 1 && escape_.ptr()[0] == '\\'; }
  const char *data_;
  int64_t length_;
  int64_t pos_;
  int64_t line_;
  ObString delimiter_;
  ObString quote_;
  ObString escape_;
};

struct ColumnState
{
  ObObjType type_;
  int32_t max_length_;
  bool has_null_;
  ColumnState() : type_(ObNullType), max_length_(0), has_null_(false) {}
};

int build_column(const ObString &name, const ColumnState &state, ObIAllocator &allocator,
                 FileTableColumn &column)
{
  int ret = OB_SUCCESS;
  OZ (file_table_copy_string(allocator, name.ptr(), name.length(), column.name_));
  column.type_ = state.type_ == ObNullType ? ObVarcharType : state.type_;
  column.length_ = state.max_length_ < 16 ? 16 : static_cast<int32_t>(
      MIN(static_cast<int64_t>(state.max_length_), FILE_TABLE_MAX_VARCHAR_LENGTH));
  // 只有出现过 NULL 的列才允许为 NULL，用于 DESCRIBE 的 null 列。
  column.nullable_ = state.has_null_;
  return ret;
}

// 表头字段归一化：去掉首尾空白，空名用 columnN 兜底。
int normalize_header_name(const ObString &raw, const int64_t index, ObIAllocator &allocator,
                          ObString &name)
{
  int ret = OB_SUCCESS;
  int64_t begin = 0;
  int64_t end = raw.length();
  while (begin < end && (raw.ptr()[begin] == ' ' || raw.ptr()[begin] == '\t')) { ++begin; }
  while (end > begin && (raw.ptr()[end - 1] == ' ' || raw.ptr()[end - 1] == '\t')) { --end; }
  const ObString trimmed(static_cast<int32_t>(end - begin), raw.ptr() + begin);
  if (trimmed.empty()) {
    char buffer[32];
    const int32_t length = static_cast<int32_t>(snprintf(buffer, sizeof(buffer), "column%d",
                                                         static_cast<int>(index + 1)));
    OZ (file_table_copy_string(allocator, buffer, length, name));
  } else {
    OZ (file_table_copy_string(allocator, trimmed.ptr(), trimmed.length(), name));
  }
  return ret;
}

// 按表头生成列定义：重名追加 __2/__3（DuckDB 风格）。绑定期推断和执行期校验共用这套规则，
// 否则像 "id,name,...,id" 这种含重名列的表头会被误判为"表头已变化"。
int build_header_columns(const ObIArray<RawField> &fields, ObIAllocator &allocator,
                         ObIArray<FileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
    ObString base;
    OZ (normalize_header_name(fields.at(i).text_, i, allocator, base));
    ObString name = base;
    bool clash = true;
    int64_t dup = 1;
    while (OB_SUCC(ret) && clash) {
      clash = false;
      for (int64_t j = 0; j < columns.count(); ++j) {
        if (ObCharset::case_insensitive_equal(name, columns.at(j).name_)) {
          clash = true;
          break;
        }
      }
      if (OB_SUCC(ret) && clash) {
        ++dup;
        const int64_t need = base.length() + 24;
        char *buffer = static_cast<char *>(allocator.alloc(need));
        if (buffer == nullptr) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buffer, base.ptr(), base.length());
          const int32_t suffix_length = static_cast<int32_t>(snprintf(
              buffer + base.length(), need - base.length(), "__%d", static_cast<int>(dup)));
          name.assign_ptr(buffer, static_cast<int32_t>(base.length() + suffix_length));
        }
      }
    }
    if (OB_SUCC(ret)) {
      FileTableColumn column;
      column.name_ = name;
      OZ (columns.push_back(column));
    }
  }
  return ret;
}

bool is_null_text(const ObString &null_str, const RawField &field)
{
  return !null_str.empty() && !field.quoted_ && null_str.length() == field.text_.length() &&
         0 == strncmp(null_str.ptr(), field.text_.ptr(), field.text_.length());
}

class CsvRowSource final : public FileTableRowSource
{
public:
  CsvRowSource(const FileTableDefinition &definition, ObIAllocator &allocator)
    : definition_(definition), allocator_(allocator), files_(), file_index_(0), data_(nullptr),
      length_(0), parser_(nullptr), header_checked_(false)
  {}
  ~CsvRowSource() override { (void) close(); }
  int open()
  {
    int ret = OB_SUCCESS;
    OZ (resolve_files(definition_.path_, allocator_, files_));
    if (OB_SUCC(ret)) {
      OZ (open_file(0));
    }
    return ret;
  }
  int next_row(ObIArray<ObObj> &values) override
  {
    int ret = OB_SUCCESS;
    values.reuse();
    ObSEArray<RawField, 16> fields;
    bool produced = false;
    while (OB_SUCC(ret) && !produced) {
      if (data_ == nullptr) {
        ret = OB_ITER_END;
      } else {
        int64_t line = 0;
        ret = parser_->next_record(allocator_, fields, line);
        if (OB_SUCC(ret) && !header_checked_) {
          if (definition_.has_header_) {
            OZ (verify_header(fields, line));
          } else if (fields.count() != definition_.columns_.count()) {
            ret = OB_INVALID_DATA;
            LOG_USER_ERROR(OB_INVALID_DATA, "CSV field count does not match the inferred schema");
          }
          header_checked_ = true;
        } else if (OB_SUCC(ret)) {
          OZ (convert(fields, values, line));
          produced = true;
        }
      }
      if (ret == OB_ITER_END && file_index_ + 1 < files_.count()) {
        ret = open_file(++file_index_);
      }
    }
    return ret;
  }
  int close() override
  {
    data_ = nullptr;
    length_ = 0;
    parser_ = nullptr;
    return OB_SUCCESS;
  }
private:
  int open_file(const int64_t index)
  {
    int ret = OB_SUCCESS;
    if (index < 0 || index >= files_.count()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      OZ (read_file(files_.at(index), allocator_, data_, length_));
      if (OB_SUCC(ret)) {
        header_checked_ = false;
        parser_ = static_cast<CsvParser *>(allocator_.alloc(sizeof(CsvParser)));
        if (parser_ == nullptr) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          new (parser_) CsvParser(data_, length_, definition_.delimiter_, definition_.quote_,
                                  definition_.escape_);
        }
      }
    }
    return ret;
  }
  int verify_header(const ObIArray<RawField> &fields, const int64_t line)
  {
    int ret = OB_SUCCESS;
    ObSEArray<FileTableColumn, 16> expected;
    if (fields.count() != definition_.columns_.count()) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA, "CSV header does not match the schema inferred at bind");
    } else if (OB_FAIL(build_header_columns(fields, allocator_, expected))) {
    } else if (expected.count() != definition_.columns_.count()) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA, "CSV header does not match the schema inferred at bind");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < expected.count(); ++i) {
      if (!ObCharset::case_insensitive_equal(expected.at(i).name_, definition_.columns_.at(i).name_)) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA, "CSV header changed after the statement was bound");
      }
    }
    UNUSED(line);
    return ret;
  }
  int convert(const ObIArray<RawField> &fields, ObIArray<ObObj> &values, const int64_t line)
  {
    int ret = OB_SUCCESS;
    if (fields.count() != definition_.columns_.count()) {
      if (definition_.ignore_errors_) {
        LOG_WARN("skip malformed CSV record", K(line), "fields", fields.count(),
                 "columns", definition_.columns_.count());
        ret = OB_ITER_END;
      } else {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA, "malformed CSV record, ignore_errors => true skips it");
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < definition_.columns_.count(); ++i) {
        ObObj value;
        const FileTableColumn &column = definition_.columns_.at(i);
        const RawField &field = fields.at(i);
        if ((!field.quoted_ && field.text_.empty()) || is_null_text(definition_.null_str_, field)) {
          value.set_null();
        } else if (OB_FAIL(convert_text_to_obj(column.type_, field.text_, allocator_, value))) {
          if (definition_.ignore_errors_) {
            ret = OB_SUCCESS;
            value.set_null();
          } else {
            LOG_USER_ERROR(OB_INVALID_DATA, "value does not match the inferred column type");
          }
        }
        if (OB_SUCC(ret)) {
          OZ (values.push_back(value));
        }
      }
    }
    return ret;
  }
  const FileTableDefinition &definition_;
  ObIAllocator &allocator_;
  ObSEArray<ObString, 8> files_;
  int64_t file_index_;
  char *data_;
  int64_t length_;
  CsvParser *parser_;
  bool header_checked_;
};

struct JsonValue
{
  ObString text_;
  ObObjType type_;
  JsonValue() : text_(), type_(ObNullType) {}
};

int parse_json_key(const char *data, const int64_t length, int64_t &pos, ObIAllocator &allocator,
                   ObString &key)
{
  int ret = OB_SUCCESS;
  char *buffer = static_cast<char *>(allocator.alloc(length - pos + 1));
  int64_t write = 0;
  bool closed = false;
  if (buffer == nullptr) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  while (OB_SUCC(ret) && pos < length && !closed) {
    const char c = data[pos];
    if (c == '\\' && pos + 1 < length) {
      const char e = data[pos + 1];
      char decoded = e;
      if (e == 'n') { decoded = '\n'; }
      else if (e == 't') { decoded = '\t'; }
      else if (e == 'r') { decoded = '\r'; }
      else if (e == 'b') { decoded = '\b'; }
      else if (e == 'f') { decoded = '\f'; }
      buffer[write++] = decoded;
      pos += 2;
    } else if (c == '"') {
      ++pos;
      closed = true;
    } else {
      buffer[write++] = c;
      ++pos;
    }
  }
  if (OB_SUCC(ret)) {
    if (!closed) {
      ret = OB_INVALID_DATA;
    } else {
      key.assign_ptr(buffer, static_cast<int32_t>(write));
    }
  }
  return ret;
}

// 单行 JSON 对象解析；嵌套对象/数组直接拒绝（题目要求）。
int parse_json_line(const char *data, const int64_t length, int64_t &pos, ObIAllocator &allocator,
                    ObIArray<ObString> &keys, ObIArray<JsonValue> &values)
{
  int ret = OB_SUCCESS;
  while (pos < length && (data[pos] == ' ' || data[pos] == '\t' ||
                          data[pos] == '\r' || data[pos] == '\n')) { ++pos; }
  if (pos >= length) {
    ret = OB_ITER_END;
  } else if (data[pos] != '{') {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "JSON row must be an object");
  } else {
    ++pos;
    while (pos < length && (data[pos] == ' ' || data[pos] == '\t')) { ++pos; }
    if (pos < length && data[pos] == '}') {
      ++pos;
    } else {
      while (OB_SUCC(ret)) {
        while (pos < length && (data[pos] == ' ' || data[pos] == '\t')) { ++pos; }
        if (pos >= length || data[pos] != '"') {
          ret = OB_INVALID_DATA;
        } else {
          ++pos;
          ObString key;
          JsonValue value;
          OZ (parse_json_key(data, length, pos, allocator, key));
          while (OB_SUCC(ret) && pos < length && (data[pos] == ' ' || data[pos] == '\t')) { ++pos; }
          if (OB_SUCC(ret) && (pos >= length || data[pos] != ':')) {
            ret = OB_INVALID_DATA;
          } else {
            ++pos;
          }
          while (OB_SUCC(ret) && pos < length && (data[pos] == ' ' || data[pos] == '\t')) { ++pos; }
          if (OB_SUCC(ret) && pos >= length) {
            ret = OB_INVALID_DATA;
          } else if (OB_SUCC(ret)) {
            const char c = data[pos];
            if (c == '{' || c == '[') {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested JSON object or array");
            } else if (c == '"') {
              ++pos;
              ObString text;
              OZ (parse_json_key(data, length, pos, allocator, text));
              value.text_ = text;
              value.type_ = ObVarcharType;
            } else {
              const int64_t start = pos;
              while (pos < length && data[pos] != ',' && data[pos] != '}' &&
                     data[pos] != '\r' && data[pos] != '\n') { ++pos; }
              ObString raw(static_cast<int32_t>(pos - start), data + start);
              while (raw.length() > 0 &&
                     (raw.ptr()[raw.length() - 1] == ' ' || raw.ptr()[raw.length() - 1] == '\t')) {
                raw.assign_ptr(raw.ptr(), raw.length() - 1);
              }
              if (4 == raw.length() && 0 == strncmp(raw.ptr(), "null", 4)) {
                value.type_ = ObNullType;
              } else if ((4 == raw.length() && 0 == strncmp(raw.ptr(), "true", 4)) ||
                         (5 == raw.length() && 0 == strncmp(raw.ptr(), "false", 5))) {
                value.type_ = ObTinyIntType;
                value.text_ = raw;
              } else {
                bool is_null = false;
                value.type_ = detect_file_value_type(raw, true, &is_null);
                if (is_null) { value.type_ = ObNullType; }
                value.text_ = raw;
              }
            }
          }
          if (OB_SUCC(ret)) {
            OZ (keys.push_back(key));
            OZ (values.push_back(value));
            while (pos < length && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r')) { ++pos; }
            if (pos < length && data[pos] == ',') {
              ++pos;
              continue;
            } else if (pos < length && data[pos] == '}') {
              ++pos;
              break;
            } else {
              ret = OB_INVALID_DATA;
            }
          }
        }
      }
    }
  }
  return ret;
}

class JsonRowSource final : public FileTableRowSource
{
public:
  JsonRowSource(const FileTableDefinition &definition, ObIAllocator &allocator)
    : definition_(definition), allocator_(allocator), files_(), file_index_(0), data_(nullptr),
      length_(0), pos_(0)
  {}
  ~JsonRowSource() override { (void) close(); }
  int open()
  {
    int ret = OB_SUCCESS;
    OZ (resolve_files(definition_.path_, allocator_, files_));
    if (OB_SUCC(ret)) { ret = open_file(0); }
    return ret;
  }
  int next_row(ObIArray<ObObj> &values) override
  {
    int ret = OB_SUCCESS;
    values.reuse();
    bool produced = false;
    while (OB_SUCC(ret) && !produced) {
      if (data_ == nullptr) {
        ret = OB_ITER_END;
      } else {
        while (pos_ < length_ && (data_[pos_] == '\n' || data_[pos_] == '\r' ||
                                  data_[pos_] == ' ' || data_[pos_] == '\t')) { ++pos_; }
        if (pos_ >= length_) {
          ret = OB_ITER_END;
        } else {
          ObSEArray<ObString, 32> keys;
          ObSEArray<JsonValue, 32> raw_values;
          OZ (parse_json_line(data_, length_, pos_, allocator_, keys, raw_values));
          for (int64_t i = 0; OB_SUCC(ret) && i < definition_.columns_.count(); ++i) {
            const FileTableColumn &column = definition_.columns_.at(i);
            ObObj value;
            value.set_null();
            for (int64_t j = 0; j < keys.count(); ++j) {
              if (ObCharset::case_insensitive_equal(keys.at(j), column.name_)) {
                const JsonValue &raw = raw_values.at(j);
                if (raw.type_ != ObNullType &&
                    OB_FAIL(convert_text_to_obj(column.type_, raw.text_, allocator_, value))) {
                  if (definition_.ignore_errors_) {
                    ret = OB_SUCCESS;
                    value.set_null();
                  }
                }
                break;
              }
            }
            if (OB_SUCC(ret)) { OZ (values.push_back(value)); }
          }
          produced = OB_SUCC(ret);
        }
      }
      if (ret == OB_ITER_END && file_index_ + 1 < files_.count()) {
        ret = open_file(++file_index_);
      }
    }
    return ret;
  }
  int close() override
  {
    data_ = nullptr;
    length_ = 0;
    pos_ = 0;
    return OB_SUCCESS;
  }
private:
  int open_file(const int64_t index)
  {
    int ret = OB_SUCCESS;
    if (index < 0 || index >= files_.count()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      OZ (read_file(files_.at(index), allocator_, data_, length_));
      if (OB_SUCC(ret)) { pos_ = 0; }
    }
    return ret;
  }
  const FileTableDefinition &definition_;
  ObIAllocator &allocator_;
  ObSEArray<ObString, 8> files_;
  int64_t file_index_;
  char *data_;
  int64_t length_;
  int64_t pos_;
};

int infer_csv_columns(FileTableDefinition &definition, ObIAllocator &allocator,
                      ObIArray<FileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 8> files;
  OZ (resolve_files(definition.path_, allocator, files));
  if (OB_SUCC(ret)) {
    char *data = nullptr;
    int64_t length = 0;
    OZ (read_file(files.at(0), allocator, data, length));
    FileStat info;
    OZ (stat_file(files.at(0), info));
    if (OB_SUCC(ret)) {
      definition.file_size_ = info.size_;
      definition.file_mtime_ = info.mtime_;
    }
    CsvParser parser(data, length, definition.delimiter_, definition.quote_, definition.escape_);
    ObSEArray<RawField, 16> fields;
    ObSEArray<ColumnState, 16> states;
    int64_t line = 0;
    int64_t rows = 0;
    // 表头：列名、去重、空名兜底
    if (OB_SUCC(ret) && definition.has_header_) {
      OZ (parser.next_record(allocator, fields, line));
      if (OB_SUCC(ret) && fields.empty()) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA, "CSV file has no header line");
      }
      OZ (build_header_columns(fields, allocator, columns));
      for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
        ColumnState state;
        OZ (states.push_back(state));
      }
    } else if (OB_SUCC(ret)) {
      // 无表头：按第一行的字段数生成 column1..N，并把这一行计入推断
      OZ (parser.next_record(allocator, fields, line));
      for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
        char buffer[32];
        const int32_t text_length = static_cast<int32_t>(snprintf(buffer, sizeof(buffer), "column%d",
                                                                  static_cast<int>(i + 1)));
        ColumnState state;
        FileTableColumn column;
        OZ (file_table_copy_string(allocator, buffer, text_length, column.name_));
        OZ (states.push_back(state));
        OZ (columns.push_back(column));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
        bool is_null = false;
        const ObObjType type = detect_file_value_type(fields.at(i).text_, fields.at(i).quoted_, &is_null);
        if (is_null || is_null_text(definition.null_str_, fields.at(i))) {
          states.at(i).has_null_ = true;
        } else {
          states.at(i).type_ = widen_file_table_type(states.at(i).type_, type);
        }
        states.at(i).max_length_ = static_cast<int32_t>(
            MAX(static_cast<int64_t>(states.at(i).max_length_), fields.at(i).text_.length()));
      }
      ++rows;
    }
    while (OB_SUCC(ret) && (definition.sample_size_ <= 0 || rows < definition.sample_size_)) {
      if (OB_FAIL(parser.next_record(allocator, fields, line))) {
        if (ret == OB_ITER_END) {
          ret = OB_SUCCESS;
          break;  // 文件读完必须跳出，否则采样上限未设置时会无限循环
        }
      } else if (fields.count() != columns.count()) {
        if (!definition.ignore_errors_) {
          ret = OB_INVALID_DATA;
          LOG_USER_ERROR(OB_INVALID_DATA, "malformed CSV record while inferring column types");
        }
      } else {
        for (int64_t i = 0; i < fields.count(); ++i) {
          bool is_null = false;
          const ObObjType type = detect_file_value_type(fields.at(i).text_, fields.at(i).quoted_, &is_null);
          if (is_null || is_null_text(definition.null_str_, fields.at(i))) {
            states.at(i).has_null_ = true;
          } else {
            states.at(i).type_ = widen_file_table_type(states.at(i).type_, type);
          }
          states.at(i).max_length_ = static_cast<int32_t>(
              MAX(static_cast<int64_t>(states.at(i).max_length_), fields.at(i).text_.length()));
        }
        ++rows;
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      ObString name = columns.at(i).name_;
      OZ (build_column(name, states.at(i), allocator, columns.at(i)));
    }
  }
  return ret;
}

// glob(pattern)：返回匹配到的文件全路径，单列 file。
int infer_glob_columns(FileTableDefinition &definition, ObIAllocator &allocator,
                       ObIArray<FileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  FileTableColumn column;
  OZ (file_table_copy_string(allocator, "file", 4, column.name_));
  column.type_ = ObVarcharType;
  column.length_ = 1024;
  column.nullable_ = false;
  OZ (columns.push_back(column));
  UNUSED(definition);
  return ret;
}

// glob 表函数：每次打开时展开通配符，按文件名排序后逐行给出完整路径。
class GlobRowSource final : public FileTableRowSource
{
public:
  GlobRowSource(const FileTableDefinition &definition, ObIAllocator &allocator)
    : definition_(definition), allocator_(allocator), files_(), index_(0)
  {}
  ~GlobRowSource() override { (void) close(); }
  int open() override
  {
    int ret = OB_SUCCESS;
    files_.reuse();
    index_ = 0;
    OZ (resolve_files(definition_.path_, allocator_, files_));
    return ret;
  }
  int next_row(ObIArray<ObObj> &values) override
  {
    int ret = OB_SUCCESS;
    values.reuse();
    if (index_ >= files_.count()) {
      ret = OB_ITER_END;
    } else {
      ObObj value;
      OZ (make_varchar(files_.at(index_), allocator_, value));
      OZ (values.push_back(value));
      ++index_;
    }
    return ret;
  }
  int close() override
  {
    index_ = 0;
    return OB_SUCCESS;
  }
private:
  const FileTableDefinition &definition_;
  ObIAllocator &allocator_;
  ObSEArray<ObString, 16> files_;
  int64_t index_;
};

int infer_json_columns(FileTableDefinition &definition, ObIAllocator &allocator,
                       ObIArray<FileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 8> files;
  OZ (resolve_files(definition.path_, allocator, files));
  if (OB_SUCC(ret)) {
    ObSEArray<ObString, 64> names;
    ObSEArray<ColumnState, 64> states;
    for (int64_t f = 0; OB_SUCC(ret) && f < files.count(); ++f) {
      char *data = nullptr;
      int64_t length = 0;
      OZ (read_file(files.at(f), allocator, data, length));
      if (OB_SUCC(ret) && f == 0) {
        FileStat info;
        OZ (stat_file(files.at(f), info));
        if (OB_SUCC(ret)) {
          definition.file_size_ = info.size_;
          definition.file_mtime_ = info.mtime_;
        }
      }
      int64_t pos = 0;
      int64_t rows = 0;
      while (OB_SUCC(ret) && (definition.sample_size_ <= 0 || rows < definition.sample_size_)) {
        if (pos >= length) { break; }
        ObSEArray<ObString, 32> keys;
        ObSEArray<JsonValue, 32> raw_values;
        if (OB_FAIL(parse_json_line(data, length, pos, allocator, keys, raw_values))) {
          if (ret == OB_ITER_END) {
            ret = OB_SUCCESS;
            break;
          }
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
            int64_t index = -1;
            for (int64_t j = 0; index < 0 && j < names.count(); ++j) {
              if (ObCharset::case_insensitive_equal(names.at(j), keys.at(i))) { index = j; }
            }
            if (index < 0) {
              ColumnState state;
              OZ (names.push_back(keys.at(i)));
              OZ (states.push_back(state));
              index = names.count() - 1;
            }
            const JsonValue &value = raw_values.at(i);
            if (value.type_ != ObNullType) {
              states.at(index).type_ = widen_file_table_type(states.at(index).type_, value.type_);
            }
            states.at(index).max_length_ = static_cast<int32_t>(
                MAX(static_cast<int64_t>(states.at(index).max_length_), value.text_.length()));
          }
          ++rows;
        }
      }
    }
    if (OB_SUCC(ret) && names.empty()) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA, "JSON file has no readable row");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
      FileTableColumn column;
      OZ (build_column(names.at(i), states.at(i), allocator, column));
      OZ (columns.push_back(column));
    }
  }
  return ret;
}

} // namespace file_table_reader_impl

using namespace file_table_reader_impl;

int file_table_stat(const ObString &path, int64_t &size, int64_t &mtime)
{
  int ret = OB_SUCCESS;
  FileStat info;
  OZ (stat_file(path, info));
  if (OB_SUCC(ret)) {
    size = info.size_;
    mtime = info.mtime_;
  }
  return ret;
}

int infer_file_table_columns(FileTableDefinition &definition, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  ObSEArray<FileTableColumn, 16> columns;
  definition.columns_.reuse();
  if (OB_FAIL(check_file_table_path(definition.path_))) {
  } else if (definition.kind_ == FILE_TABLE_CSV) {
    OZ (infer_csv_columns(definition, allocator, columns));
  } else if (definition.kind_ == FILE_TABLE_JSON) {
    OZ (infer_json_columns(definition, allocator, columns));
  } else if (definition.kind_ == FILE_TABLE_GLOB) {
    OZ (infer_glob_columns(definition, allocator, columns));
  } else {
    OZ (infer_parquet_columns(definition, allocator));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    OZ (definition.columns_.push_back(columns.at(i)));
  }
  if (OB_SUCC(ret) && definition.file_size_ < 0) {
    // 记录文件签名用于执行期变化检测；glob 路径没有单一文件签名，忽略即可。
    int64_t size = -1;
    int64_t mtime = -1;
    if (OB_SUCCESS == file_table_stat(definition.path_, size, mtime)) {
      definition.file_size_ = size;
      definition.file_mtime_ = mtime;
    }
  }
  if (OB_SUCC(ret) && definition.columns_.empty()) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "no column inferred from file");
  }
  return ret;
}

int create_file_table_row_source(const FileTableDefinition &definition,
                                 ObIAllocator &allocator,
                                 FileTableRowSource *&source)
{
  int ret = OB_SUCCESS;
  source = nullptr;
  FileTableRowSource *result = nullptr;
  if (definition.kind_ == FILE_TABLE_CSV) {
    void *memory = allocator.alloc(sizeof(CsvRowSource));
    if (memory == nullptr) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      result = new (memory) CsvRowSource(definition, allocator);
    }
  } else if (definition.kind_ == FILE_TABLE_JSON) {
    void *memory = allocator.alloc(sizeof(JsonRowSource));
    if (memory == nullptr) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      result = new (memory) JsonRowSource(definition, allocator);
    }
  } else if (definition.kind_ == FILE_TABLE_GLOB) {
    void *memory = allocator.alloc(sizeof(GlobRowSource));
    if (memory == nullptr) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      result = new (memory) GlobRowSource(definition, allocator);
    }
  } else {
    OZ (create_parquet_row_source(definition, allocator, result));
  }
  if (OB_SUCC(ret)) { source = result; }
  return ret;
}

} // namespace sql
} // namespace oceanbase
