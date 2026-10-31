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

// read_parquet 的最小实现：自解析 Parquet Footer（thrift compact protocol），
// 支持扁平 schema、多 Row Group、可空列、PLAIN / RLE_DICTIONARY 编码，
// 以及 UNCOMPRESSED / SNAPPY 两种压缩。

#define USING_LOG_PREFIX SQL_ENG
#include "sql/table_function/file_table_function.h"
#include "sql/table_function/table_function.h"
#include "lib/utility/ob_print_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace oceanbase {
namespace sql {
using namespace common;

namespace parquet_impl {

// ---------------------------------------------------------------------------
// Thrift compact protocol 读取器
// ---------------------------------------------------------------------------
enum CompactType : int8_t {
  CT_STOP = 0,
  CT_TRUE = 1,
  CT_FALSE = 2,
  CT_BYTE = 3,
  CT_I16 = 4,
  CT_I32 = 5,
  CT_I64 = 6,
  CT_DOUBLE = 7,
  CT_BINARY = 8,
  CT_LIST = 9,
  CT_SET = 10,
  CT_MAP = 11,
  CT_STRUCT = 12,
};

class CompactReader
{
public:
  CompactReader(const uint8_t *data, const int64_t length)
    : data_(data), length_(length), pos_(0), last_id_(0), depth_(0), stack_()
  {}
  int read_field_header(int8_t &type, int16_t &id)
  {
    int ret = OB_SUCCESS;
    type = CT_STOP;
    id = 0;
    if (pos_ >= length_) {
      ret = OB_INVALID_DATA;
    } else {
      const uint8_t byte = data_[pos_++];
      const uint8_t delta = byte >> 4;
      const int8_t field_type = static_cast<int8_t>(byte & 0x0f);
      if (field_type == CT_STOP) {
        type = CT_STOP;
        last_id_ = 0;
      } else if (delta == 0) {
        int32_t absolute = 0;
        if (OB_FAIL(read_varint32(absolute))) {
        } else {
          id = static_cast<int16_t>(absolute);
          last_id_ = id;
          type = field_type == CT_TRUE ? CT_TRUE : field_type;
        }
      } else {
        last_id_ = static_cast<int16_t>(last_id_ + delta);
        id = last_id_;
        type = field_type;
      }
    }
    return ret;
  }
  int read_i32(int32_t &value)
  {
    int64_t raw = 0;
    int ret = read_varint64(raw);
    if (OB_SUCC(ret)) {
      value = static_cast<int32_t>((raw >> 1) ^ -(raw & 1));
    }
    return ret;
  }
  int read_i64(int64_t &value)
  {
    int64_t raw = 0;
    int ret = read_varint64(raw);
    if (OB_SUCC(ret)) {
      value = (raw >> 1) ^ -(raw & 1);
    }
    return ret;
  }
  // compact protocol 中 bool 的取值编码在字段头的 type nibble 里（CT_TRUE / CT_FALSE），
  // 后面不再跟随取值字节；这里直接由字段类型判断。
  bool is_true_field(const int8_t type) const { return type == CT_TRUE; }
  int read_binary(ObIAllocator &allocator, ObString &value)
  {
    int ret = OB_SUCCESS;
    int64_t size = 0;
    if (OB_FAIL(read_varint64(size))) {
    } else if (size < 0 || pos_ + size > length_) {
      ret = OB_INVALID_DATA;
    } else {
      char *copy = nullptr;
      if (size > 0 && (copy = static_cast<char *>(allocator.alloc(size))) == nullptr) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (size > 0) {
        MEMCPY(copy, data_ + pos_, size);
      }
      if (OB_SUCC(ret)) {
        value.assign_ptr(copy, static_cast<int32_t>(size));
        pos_ += size;
      }
    }
    return ret;
  }
  int read_double(double &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ + 8 > length_) {
      ret = OB_INVALID_DATA;
    } else {
      uint8_t bytes[8];
      MEMCPY(bytes, data_ + pos_, 8);
      pos_ += 8;
      // compact protocol 中 double 为大端
      uint64_t bits = 0;
      for (int32_t i = 0; i < 8; ++i) {
        bits = (bits << 8) | bytes[i];
      }
      MEMCPY(&value, &bits, 8);
    }
    return ret;
  }
  int read_list_begin(int8_t &elem_type, int64_t &size)
  {
    int ret = OB_SUCCESS;
    elem_type = CT_STOP;
    size = 0;
    if (pos_ >= length_) {
      ret = OB_INVALID_DATA;
    } else {
      const uint8_t byte = data_[pos_++];
      elem_type = static_cast<int8_t>(byte & 0x0f);
      size = (byte >> 4) & 0x0f;
      if (size == 15) {
        if (OB_FAIL(read_varint64(size))) {
        }
      }
      if (OB_SUCC(ret) && (size < 0 || size >= 0x7fffffff)) {
        ret = OB_INVALID_DATA;
      }
    }
    return ret;
  }
  int read_struct_begin()
  {
    int ret = OB_SUCCESS;
    if (depth_ >= 32) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(stack_.push_back(last_id_))) {
    } else {
      last_id_ = 0;
      ++depth_;
    }
    return ret;
  }
  int read_struct_end()
  {
    int ret = OB_SUCCESS;
    if (stack_.empty()) {
      ret = OB_INVALID_DATA;
    } else {
      last_id_ = stack_.at(stack_.count() - 1);
      (void) stack_.pop_back();
      --depth_;
    }
    return ret;
  }
  bool eof() const { return pos_ >= length_; }
  int64_t pos() const { return pos_; }
  int skip_field(const int8_t type)
  {
    int ret = OB_SUCCESS;
    switch (type) {
      case CT_TRUE:
      case CT_FALSE:
        break;
      case CT_BYTE:
        if (pos_ + 1 > length_) { ret = OB_INVALID_DATA; } else { pos_ += 1; }
        break;
      case CT_I16:
      case CT_I32:
      case CT_I64: {
        int64_t raw = 0;
        OZ (read_varint64(raw));
        break;
      }
      case CT_DOUBLE:
        if (pos_ + 8 > length_) { ret = OB_INVALID_DATA; } else { pos_ += 8; }
        break;
      case CT_BINARY: {
        int64_t size = 0;
        OZ (read_varint64(size));
        if (OB_SUCC(ret) && (size < 0 || pos_ + size > length_)) {
          ret = OB_INVALID_DATA;
        } else {
          pos_ += size;
        }
        break;
      }
      case CT_LIST:
      case CT_SET: {
        int8_t elem_type = CT_STOP;
        int64_t size = 0;
        OZ (read_list_begin(elem_type, size));
        for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          OZ (skip_field(elem_type));
        }
        break;
      }
      case CT_MAP: {
        int64_t size = 0;
        if (pos_ >= length_) {
          ret = OB_INVALID_DATA;
        } else {
          const uint8_t byte = data_[pos_++];
          const int8_t key_type = static_cast<int8_t>(byte >> 4);
          const int8_t value_type = static_cast<int8_t>(byte & 0x0f);
          size = byte & 0xff;
          if (size == 0xff) {
            // 不常见，忽略
            ret = OB_NOT_SUPPORTED;
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            OZ (skip_field(key_type));
            OZ (skip_field(value_type));
          }
        }
        break;
      }
      case CT_STRUCT: {
        OZ (read_struct_begin());
        int8_t inner_type = CT_STOP;
        int16_t inner_id = 0;
        while (OB_SUCC(ret)) {
          OZ (read_field_header(inner_type, inner_id));
          if (OB_SUCC(ret)) {
            if (inner_type == CT_STOP) { break; }
            OZ (skip_field(inner_type));
          }
        }
        if (OB_SUCC(ret)) { OZ (read_struct_end()); }
        break;
      }
      default:
        ret = OB_INVALID_DATA;
        break;
    }
    return ret;
  }
private:
  int read_varint64(int64_t &value)
  {
    int ret = OB_SUCCESS;
    uint64_t result = 0;
    int32_t shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      if (pos_ >= length_ || shift > 63) {
        ret = OB_INVALID_DATA;
      } else {
        const uint8_t byte = data_[pos_++];
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        shift += 7;
        done = (byte & 0x80) == 0;
      }
    }
    if (OB_SUCC(ret)) { value = static_cast<int64_t>(result); }
    return ret;
  }
  int read_varint32(int32_t &value)
  {
    int64_t raw = 0;
    int ret = read_varint64(raw);
    if (OB_SUCC(ret)) { value = static_cast<int32_t>(raw); }
    return ret;
  }
  const uint8_t *data_;
  int64_t length_;
  int64_t pos_;
  int16_t last_id_;
  int32_t depth_;
  ObSEArray<int16_t, 32> stack_;
};

// ---------------------------------------------------------------------------
// Parquet 元数据结构
// ---------------------------------------------------------------------------
enum ParquetPhysicalType : int32_t {
  PT_BOOLEAN = 0,
  PT_INT32 = 1,
  PT_INT64 = 2,
  PT_INT96 = 3,
  PT_FLOAT = 4,
  PT_DOUBLE = 5,
  PT_BYTE_ARRAY = 6,
  PT_FIXED_LEN_BYTE_ARRAY = 7,
};

enum ParquetConvertedType : int32_t {
  CT_UTF8 = 0,
  CT_DECIMAL = 5,
  CT_DATE = 6,
  CT_TIMESTAMP_MILLIS = 9,
  CT_TIMESTAMP_MICROS = 10,
};

struct ParquetSchemaElement
{
  int32_t type_;
  int32_t repetition_;
  int32_t converted_type_;
  int32_t type_length_;
  int32_t num_children_;
  ObString name_;
  ParquetSchemaElement()
    : type_(-1), repetition_(0), converted_type_(-1), type_length_(0), num_children_(0), name_()
  {}
};

struct ParquetColumnChunk
{
  int32_t type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  ParquetColumnChunk()
    : type_(-1), codec_(0), num_values_(0), data_page_offset_(-1),
      dictionary_page_offset_(-1)
  {}
};

struct ParquetRowGroup
{
  int64_t num_rows_;
  int64_t total_byte_size_;
  ObSEArray<ParquetColumnChunk, 8> columns_;
  ParquetRowGroup() : num_rows_(0), total_byte_size_(0), columns_() {}
};

int parse_schema_element(CompactReader &reader, ObIAllocator &allocator, ParquetSchemaElement &element)
{
  int ret = OB_SUCCESS;
  OZ (reader.read_struct_begin());
  int8_t type = CT_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.read_field_header(type, id));
    if (OB_SUCC(ret)) {
      if (type == CT_STOP) { break; }
      int32_t value = 0;
      switch (id) {
        case 1:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { element.type_ = value; }
          break;
        case 2:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { element.type_length_ = value; }
          break;
        case 3:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { element.repetition_ = value; }
          break;
        case 4:
          OZ (reader.read_binary(allocator, element.name_));
          break;
        case 5:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { element.num_children_ = value; }
          break;
        case 6:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { element.converted_type_ = value; }
          break;
        default:
          OZ (reader.skip_field(type));
          break;
      }
    }
  }
  if (OB_SUCC(ret)) { OZ (reader.read_struct_end()); }
  return ret;
}

int parse_column_meta_data(CompactReader &reader, ParquetColumnChunk &chunk)
{
  int ret = OB_SUCCESS;
  OZ (reader.read_struct_begin());
  int8_t type = CT_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.read_field_header(type, id));
    if (OB_SUCC(ret)) {
      if (type == CT_STOP) { break; }
      int32_t value = 0;
      int64_t big = 0;
      switch (id) {
        case 1:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { chunk.type_ = value; }
          break;
        case 4:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { chunk.codec_ = value; }
          break;
        case 5:
          OZ (reader.read_i64(big));
          if (OB_SUCC(ret)) { chunk.num_values_ = big; }
          break;
        case 9:
          OZ (reader.read_i64(big));
          if (OB_SUCC(ret)) { chunk.data_page_offset_ = big; }
          break;
        case 11:
          OZ (reader.read_i64(big));
          if (OB_SUCC(ret)) { chunk.dictionary_page_offset_ = big; }
          break;
        default:
          OZ (reader.skip_field(type));
          break;
      }
    }
  }
  if (OB_SUCC(ret)) { OZ (reader.read_struct_end()); }
  return ret;
}

int parse_column_chunk(CompactReader &reader, ParquetColumnChunk &chunk)
{
  int ret = OB_SUCCESS;
  OZ (reader.read_struct_begin());
  int8_t type = CT_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.read_field_header(type, id));
    if (OB_SUCC(ret)) {
      if (type == CT_STOP) { break; }
      if (3 == id) {
        OZ (parse_column_meta_data(reader, chunk));
      } else {
        OZ (reader.skip_field(type));
      }
    }
  }
  if (OB_SUCC(ret)) { OZ (reader.read_struct_end()); }
  return ret;
}

int parse_row_group(CompactReader &reader, ParquetRowGroup &group)
{
  int ret = OB_SUCCESS;
  OZ (reader.read_struct_begin());
  int8_t type = CT_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.read_field_header(type, id));
    if (OB_SUCC(ret)) {
      if (type == CT_STOP) { break; }
      int64_t big = 0;
      if (1 == id) {
        int8_t elem_type = CT_STOP;
        int64_t size = 0;
        OZ (reader.read_list_begin(elem_type, size));
        for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          ParquetColumnChunk chunk;
          OZ (parse_column_chunk(reader, chunk));
          if (OB_SUCC(ret)) { OZ (group.columns_.push_back(chunk)); }
        }
      } else if (2 == id) {
        OZ (reader.read_i64(big));
        if (OB_SUCC(ret)) { group.total_byte_size_ = big; }
      } else if (3 == id) {
        OZ (reader.read_i64(big));
        if (OB_SUCC(ret)) { group.num_rows_ = big; }
      } else {
        OZ (reader.skip_field(type));
      }
    }
  }
  if (OB_SUCC(ret)) { OZ (reader.read_struct_end()); }
  return ret;
}

int parse_file_meta_data(CompactReader &reader,
                         ObIAllocator &allocator,
                         ObIArray<ParquetSchemaElement> &schema,
                         ObIArray<ParquetRowGroup> &row_groups,
                         int64_t &num_rows)
{
  int ret = OB_SUCCESS;
  OZ (reader.read_struct_begin());
  int8_t type = CT_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.read_field_header(type, id));
    if (OB_SUCC(ret)) {
      if (type == CT_STOP) { break; }
      int64_t big = 0;
      if (2 == id) {
        int8_t elem_type = CT_STOP;
        int64_t size = 0;
        OZ (reader.read_list_begin(elem_type, size));
        for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          ParquetSchemaElement element;
          OZ (parse_schema_element(reader, allocator, element));
          if (OB_SUCC(ret)) { OZ (schema.push_back(element)); }
        }
      } else if (3 == id) {
        OZ (reader.read_i64(big));
        if (OB_SUCC(ret)) { num_rows = big; }
      } else if (4 == id) {
        int8_t elem_type = CT_STOP;
        int64_t size = 0;
        OZ (reader.read_list_begin(elem_type, size));
        for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          ParquetRowGroup group;
          OZ (parse_row_group(reader, group));
          if (OB_SUCC(ret)) { OZ (row_groups.push_back(group)); }
        }
      } else {
        OZ (reader.skip_field(type));
      }
    }
  }
  if (OB_SUCC(ret)) { OZ (reader.read_struct_end()); }
  return ret;
}

// ---------------------------------------------------------------------------
// 解压：UNCOMPRESSED / SNAPPY / GZIP
// ---------------------------------------------------------------------------
int snappy_decompress(const uint8_t *input, const int64_t input_length,
                      uint8_t *output, const int64_t output_length)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  int64_t out = 0;
  // 头部 varint：解压后长度（这里只用于校验）
  {
    uint64_t declared = 0;
    int32_t shift = 0;
    bool done = false;
    while (!done && pos < input_length) {
      const uint8_t byte = input[pos++];
      declared |= static_cast<uint64_t>(byte & 0x7f) << shift;
      shift += 7;
      done = (byte & 0x80) == 0;
    }
    if (declared != static_cast<uint64_t>(output_length)) { ret = OB_INVALID_DATA; }
  }
  while (OB_SUCC(ret) && pos < input_length && out < output_length) {
    const uint8_t tag = input[pos++];
    const int32_t kind = tag & 0x03;
    if (0 == kind) {
      int64_t length = (tag >> 2) + 1;
      if (length >= 61) {
        const int32_t extra_bytes = static_cast<int32_t>(length - 60);
        length = 0;
        for (int32_t i = 0; i < extra_bytes && pos < input_length; ++i) {
          length |= static_cast<int64_t>(input[pos++]) << (8 * i);
        }
        length += 1;
      }
      if (pos + length > input_length || out + length > output_length) {
        ret = OB_INVALID_DATA;
      } else {
        MEMCPY(output + out, input + pos, length);
        pos += length;
        out += length;
      }
    } else {
      int64_t length = 0;
      int64_t offset = 0;
      if (1 == kind) {
        length = 4 + ((tag >> 2) & 0x07);
        if (pos >= input_length) { ret = OB_INVALID_DATA; break; }
        offset = ((static_cast<int64_t>(tag) & 0xe0) << 3) | input[pos++];
      } else if (2 == kind) {
        length = 1 + (tag >> 2);
        if (pos + 2 > input_length) { ret = OB_INVALID_DATA; break; }
        offset = static_cast<int64_t>(input[pos]) | (static_cast<int64_t>(input[pos + 1]) << 8);
        pos += 2;
      } else {
        length = 1 + (tag >> 2);
        if (pos + 4 > input_length) { ret = OB_INVALID_DATA; break; }
        offset = static_cast<int64_t>(input[pos]) | (static_cast<int64_t>(input[pos + 1]) << 8) |
                 (static_cast<int64_t>(input[pos + 2]) << 16) |
                 (static_cast<int64_t>(input[pos + 3]) << 24);
        pos += 4;
      }
      if (OB_SUCC(ret)) {
        if (offset <= 0 || offset > out || out + length > output_length) {
          ret = OB_INVALID_DATA;
        } else {
          for (int64_t i = 0; i < length; ++i) {
            output[out] = output[out - offset];
            ++out;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && out != output_length) { ret = OB_INVALID_DATA; }
  return ret;
}

// 未引入 zlib 依赖，GZIP 压缩的页直接报不支持，并给出明确提示。
int gzip_decompress(const uint8_t *input, const int64_t input_length,
                    uint8_t *output, const int64_t output_length)
{
  int ret = OB_NOT_SUPPORTED;
  UNUSED(input);
  UNUSED(input_length);
  UNUSED(output);
  UNUSED(output_length);
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet GZIP compression");
  return ret;
}

int decompress_page(const int32_t codec, const uint8_t *input, const int64_t input_length,
                    uint8_t *output, const int64_t output_length, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);
  switch (codec) {
    case 0:
      if (input_length != output_length) {
        ret = OB_INVALID_DATA;
      } else {
        MEMCPY(output, input, output_length);
      }
      break;
    case 1:
      OZ (snappy_decompress(input, input_length, output, output_length));
      break;
    case 2:
      OZ (gzip_decompress(input, input_length, output, output_length));
      break;
    default:
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet compression codec");
      break;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// 页解析
// ---------------------------------------------------------------------------
struct ParquetPageHeader
{
  int32_t type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int32_t num_values_;
  int32_t encoding_;
  int32_t def_level_encoding_;
  int32_t rep_level_encoding_;
  int32_t num_nulls_;
  int32_t num_rows_;
  int32_t def_levels_length_;
  int32_t rep_levels_length_;
  bool is_compressed_;
  ParquetPageHeader()
    : type_(0), uncompressed_size_(0), compressed_size_(0), num_values_(0), encoding_(0),
      def_level_encoding_(0), rep_level_encoding_(0), num_nulls_(0), num_rows_(0),
      def_levels_length_(0), rep_levels_length_(0), is_compressed_(true)
  {}
};

int parse_data_page_header(CompactReader &reader, ParquetPageHeader &header)
{
  int ret = OB_SUCCESS;
  OZ (reader.read_struct_begin());
  int8_t type = CT_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.read_field_header(type, id));
    if (OB_SUCC(ret)) {
      if (type == CT_STOP) { break; }
      int32_t value = 0;
      switch (id) {
        case 1:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.num_values_ = value; }
          break;
        case 2:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.encoding_ = value; }
          break;
        case 3:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.def_level_encoding_ = value; }
          break;
        case 4:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.rep_level_encoding_ = value; }
          break;
        default:
          OZ (reader.skip_field(type));
          break;
      }
    }
  }
  if (OB_SUCC(ret)) { OZ (reader.read_struct_end()); }
  return ret;
}

int parse_data_page_header_v2(CompactReader &reader, ParquetPageHeader &header)
{
  int ret = OB_SUCCESS;
  OZ (reader.read_struct_begin());
  int8_t type = CT_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.read_field_header(type, id));
    if (OB_SUCC(ret)) {
      if (type == CT_STOP) { break; }
      int32_t value = 0;
      switch (id) {
        case 1:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.num_values_ = value; }
          break;
        case 2:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.num_nulls_ = value; }
          break;
        case 3:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.num_rows_ = value; }
          break;
        case 4:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.encoding_ = value; }
          break;
        case 5:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.def_levels_length_ = value; }
          break;
        case 6:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.rep_levels_length_ = value; }
          break;
        case 7:
          header.is_compressed_ = reader.is_true_field(type);
          break;
        default:
          OZ (reader.skip_field(type));
          break;
      }
    }
  }
  if (OB_SUCC(ret)) { OZ (reader.read_struct_end()); }
  return ret;
}

int parse_dictionary_page_header(CompactReader &reader, ParquetPageHeader &header)
{
  int ret = OB_SUCCESS;
  OZ (reader.read_struct_begin());
  int8_t type = CT_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.read_field_header(type, id));
    if (OB_SUCC(ret)) {
      if (type == CT_STOP) { break; }
      int32_t value = 0;
      if (1 == id) {
        OZ (reader.read_i32(value));
        if (OB_SUCC(ret)) { header.num_values_ = value; }
      } else {
        OZ (reader.skip_field(type));
      }
    }
  }
  if (OB_SUCC(ret)) { OZ (reader.read_struct_end()); }
  return ret;
}

int parse_page_header(CompactReader &reader, ParquetPageHeader &header)
{
  int ret = OB_SUCCESS;
  OZ (reader.read_struct_begin());
  int8_t type = CT_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.read_field_header(type, id));
    if (OB_SUCC(ret)) {
      if (type == CT_STOP) { break; }
      int32_t value = 0;
      switch (id) {
        case 1:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.type_ = value; }
          break;
        case 2:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.uncompressed_size_ = value; }
          break;
        case 3:
          OZ (reader.read_i32(value));
          if (OB_SUCC(ret)) { header.compressed_size_ = value; }
          break;
        case 5:
          OZ (parse_data_page_header(reader, header));
          break;
        case 7:
          OZ (parse_dictionary_page_header(reader, header));
          break;
        case 8:
          OZ (parse_data_page_header_v2(reader, header));
          break;
        default:
          OZ (reader.skip_field(type));
          break;
      }
    }
  }
  if (OB_SUCC(ret)) { OZ (reader.read_struct_end()); }
  return ret;
}

// ---------------------------------------------------------------------------
// RLE / bit-packed 与 PLAIN 解码
// ---------------------------------------------------------------------------
int decode_bitpacked(const uint8_t *data, const int64_t data_length, const int32_t bit_width,
                     const int64_t count, ObIArray<int64_t> &values)
{
  int ret = OB_SUCCESS;
  int64_t bit_pos = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    int64_t value = 0;
    for (int32_t b = 0; b < bit_width; ++b) {
      const int64_t byte_index = bit_pos / 8;
      if (byte_index >= data_length) {
        ret = OB_INVALID_DATA;
        break;
      }
      const int64_t bit_index = bit_pos % 8;
      const int64_t bit = (data[byte_index] >> bit_index) & 1;
      value |= bit << b;
      ++bit_pos;
    }
    if (OB_SUCC(ret)) { OZ (values.push_back(value)); }
  }
  return ret;
}

int decode_rle_bitpacked(const uint8_t *data, const int64_t data_length, const int32_t bit_width,
                         const int64_t expected, ObIArray<int64_t> &values)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  const int32_t value_bytes = (bit_width + 7) / 8;
  while (OB_SUCC(ret) && pos < data_length && values.count() < expected) {
    const uint8_t header = data[pos++];
    if (0 == (header & 1)) {
      const int64_t run_length = header >> 1;
      int64_t value = 0;
      if (pos + value_bytes > data_length) {
        ret = OB_INVALID_DATA;
      } else {
        for (int32_t i = 0; i < value_bytes; ++i) {
          value |= static_cast<int64_t>(data[pos + i]) << (8 * i);
        }
        pos += value_bytes;
        for (int64_t i = 0; OB_SUCC(ret) && i < run_length && values.count() < expected; ++i) {
          OZ (values.push_back(value));
        }
      }
    } else {
      const int64_t groups = header >> 1;
      const int64_t count = groups * 8;
      const int64_t bytes = groups * bit_width;
      if (pos + bytes > data_length) {
        ret = OB_INVALID_DATA;
      } else {
        OZ (decode_bitpacked(data + pos, bytes, bit_width, count, values));
        pos += bytes;
        while (OB_SUCC(ret) && values.count() > expected) { (void) values.pop_back(); }
      }
    }
  }
  return ret;
}

int32_t bit_width_of(const int32_t max_value)
{
  int32_t width = 0;
  int32_t value = max_value;
  while (value > 0) {
    ++width;
    value >>= 1;
  }
  return width;
}

void map_parquet_type(const ParquetSchemaElement &element, ObObjType &type, int32_t &length)
{
  length = 0;
  switch (element.type_) {
    case PT_BOOLEAN:
      type = ObTinyIntType;
      break;
    case PT_INT32:
      type = element.converted_type_ == CT_DATE ? ObDateType : ObIntType;
      break;
    case PT_INT64:
      type = (element.converted_type_ == CT_TIMESTAMP_MILLIS ||
              element.converted_type_ == CT_TIMESTAMP_MICROS) ? ObDateTimeType : ObIntType;
      break;
    case PT_INT96:
      type = ObDateTimeType;
      break;
    case PT_FLOAT:
    case PT_DOUBLE:
      type = ObDoubleType;
      break;
    default:
      type = ObVarcharType;
      if (element.type_length_ > 0) {
        length = element.type_length_;
      }
      break;
  }
}

int read_footer(const ObString &path, ObIAllocator &allocator, const uint8_t *&footer,
                int64_t &footer_length)
{
  int ret = OB_SUCCESS;
  footer = nullptr;
  footer_length = 0;
  char name[1024];
  FILE *file = nullptr;
  struct stat st;
  if (path.length() <= 0 || path.length() >= static_cast<int32_t>(sizeof(name) - 1)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(name, path.ptr(), path.length());
    name[path.length()] = '\0';
    if (0 != ::stat(name, &st)) {
      ret = OB_FILE_NOT_EXIST;
    } else if ((file = ::fopen(name, "rb")) == nullptr) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      const int64_t file_size = static_cast<int64_t>(st.st_size);
      if (file_size < 12) {
        ret = OB_INVALID_DATA;
      } else {
        uint8_t tail[8];
        if (0 != ::fseek(file, static_cast<long>(file_size - 8), SEEK_SET) ||
            8 != static_cast<int32_t>(::fread(tail, 1, 8, file))) {
          ret = OB_IO_ERROR;
        } else if (0 != strncmp(reinterpret_cast<const char *>(tail + 4), "PAR1", 4)) {
          ret = OB_INVALID_DATA;
          LOG_USER_ERROR(OB_INVALID_DATA, "not a parquet file");
        } else {
          const int64_t meta_length = static_cast<int64_t>(tail[0]) |
                                      (static_cast<int64_t>(tail[1]) << 8) |
                                      (static_cast<int64_t>(tail[2]) << 16) |
                                      (static_cast<int64_t>(tail[3]) << 24);
          if (meta_length <= 0 || meta_length > file_size - 12) {
            ret = OB_INVALID_DATA;
          } else {
            char *buffer = static_cast<char *>(allocator.alloc(meta_length));
            if (buffer == nullptr) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (0 != ::fseek(file, static_cast<long>(file_size - 8 - meta_length), SEEK_SET) ||
                       meta_length != static_cast<int64_t>(::fread(buffer, 1, meta_length, file))) {
              ret = OB_IO_ERROR;
            } else {
              footer = reinterpret_cast<const uint8_t *>(buffer);
              footer_length = meta_length;
            }
          }
        }
      }
      (void) ::fclose(file);
    }
  }
  return ret;
}

// 把 Parquet 的单个值转换成 ObObj
int decode_plain_value(const ParquetSchemaElement &element, const ObObjType type,
                       const uint8_t *data, const int64_t data_length, int64_t &pos,
                       ObIAllocator &allocator, ObObj &obj)
{
  int ret = OB_SUCCESS;
  switch (element.type_) {
    case PT_BOOLEAN: {
      if (pos >= data_length) {
        ret = OB_INVALID_DATA;
      } else {
        const bool value = ((data[pos / 8] >> (pos % 8)) & 1) != 0;
        ++pos;
        obj.set_tinyint(value ? 1 : 0);
      }
      break;
    }
    case PT_INT32: {
      if (pos + 4 > data_length) {
        ret = OB_INVALID_DATA;
      } else {
        int32_t value = 0;
        MEMCPY(&value, data + pos, 4);
        pos += 4;
        if (type == ObDateType) {
          obj.set_date(value);
        } else {
          obj.set_int(value);
        }
      }
      break;
    }
    case PT_INT64: {
      if (pos + 8 > data_length) {
        ret = OB_INVALID_DATA;
      } else {
        int64_t value = 0;
        MEMCPY(&value, data + pos, 8);
        pos += 8;
        if (type == ObDateTimeType) {
          // TIMESTAMP_MILLIS / MICROS 统一换成秒
          obj.set_datetime(element.converted_type_ == CT_TIMESTAMP_MICROS ? value / 1000000 : value / 1000);
        } else {
          obj.set_int(value);
        }
      }
      break;
    }
    case PT_INT96: {
      if (pos + 12 > data_length) {
        ret = OB_INVALID_DATA;
      } else {
        int64_t nanos = 0;
        int32_t julian_day = 0;
        MEMCPY(&nanos, data + pos, 8);
        MEMCPY(&julian_day, data + pos + 8, 4);
        pos += 12;
        const int64_t days = static_cast<int64_t>(julian_day) - 2440588;
        obj.set_datetime(days * 86400 + nanos / 1000000000);
      }
      break;
    }
    case PT_FLOAT: {
      if (pos + 4 > data_length) {
        ret = OB_INVALID_DATA;
      } else {
        float value = 0;
        MEMCPY(&value, data + pos, 4);
        pos += 4;
        obj.set_double(static_cast<double>(value));
      }
      break;
    }
    case PT_DOUBLE: {
      if (pos + 8 > data_length) {
        ret = OB_INVALID_DATA;
      } else {
        double value = 0;
        MEMCPY(&value, data + pos, 8);
        pos += 8;
        obj.set_double(value);
      }
      break;
    }
    case PT_BYTE_ARRAY:
    case PT_FIXED_LEN_BYTE_ARRAY: {
      int64_t length = element.type_length_;
      if (element.type_ == PT_BYTE_ARRAY) {
        if (pos + 4 > data_length) {
          ret = OB_INVALID_DATA;
          break;
        }
        MEMCPY(&length, data + pos, 4);
        pos += 4;
      }
      if (OB_SUCC(ret)) {
        if (length < 0 || pos + length > data_length) {
          ret = OB_INVALID_DATA;
        } else {
          ObString text;
          OZ (file_table_copy_string(allocator, reinterpret_cast<const char *>(data + pos), length, text));
          if (OB_SUCC(ret)) {
            obj.set_varchar(text);
            obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
            pos += length;
          }
        }
      }
      break;
    }
    default:
      ret = OB_NOT_SUPPORTED;
      break;
  }
  return ret;
}

} // namespace parquet_impl

using namespace parquet_impl;

int infer_parquet_columns(FileTableDefinition &definition, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  const uint8_t *footer = nullptr;
  int64_t footer_length = 0;
  ObSEArray<ObString, 8> files;
  definition.columns_.reuse();
  if (OB_FAIL(check_file_table_path(definition.path_))) {
  } else {
    // 多文件时以首个文件的 schema 为准
    ObString probe_path = definition.path_;
    for (int64_t i = 0; OB_SUCC(ret) && i < probe_path.length(); ++i) {
      if (probe_path.ptr()[i] == '*' || probe_path.ptr()[i] == '?') {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "glob is not supported by read_parquet");
      }
    }
    OZ (read_footer(probe_path, allocator, footer, footer_length));
    if (OB_SUCC(ret)) {
      CompactReader reader(footer, footer_length);
      ObSEArray<ParquetSchemaElement, 64> schema;
      ObSEArray<ParquetRowGroup, 8> row_groups;
      int64_t num_rows = 0;
      OZ (parse_file_meta_data(reader, allocator, schema, row_groups, num_rows));
      if (OB_SUCC(ret) && schema.count() < 2) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA, "parquet schema is empty");
      }
      // schema[0] 是 root，其余为叶子列；不支持嵌套 group
      for (int64_t i = 1; OB_SUCC(ret) && i < schema.count(); ++i) {
        const ParquetSchemaElement &element = schema.at(i);
        if (element.num_children_ > 0) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested parquet schema");
        } else {
          FileTableColumn column;
          int32_t length = 0;
          map_parquet_type(element, column.type_, length);
          OZ (file_table_copy_string(allocator, element.name_.ptr(), element.name_.length(),
                                     column.name_));
          column.length_ = length > 0 ? length : 65535;
          column.nullable_ = element.repetition_ == 1;
          OZ (definition.columns_.push_back(column));
        }
      }
    }
  }
  return ret;
}

namespace parquet_impl {

class ParquetRowSource final : public FileTableRowSource
{
public:
  ParquetRowSource(const FileTableDefinition &definition, ObIAllocator &allocator)
    : definition_(definition), allocator_(allocator), file_data_(nullptr), file_length_(0),
      schema_(), row_groups_(), group_index_(-1), cells_(nullptr), group_rows_(0),
      row_in_group_(0)
  {}
  ~ParquetRowSource() override { (void) close(); }
  int open()
  {
    int ret = OB_SUCCESS;
    const uint8_t *footer = nullptr;
    int64_t footer_length = 0;
    char name[1024];
    FILE *file = nullptr;
    if (definition_.path_.length() <= 0 ||
        definition_.path_.length() >= static_cast<int32_t>(sizeof(name) - 1)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      MEMCPY(name, definition_.path_.ptr(), definition_.path_.length());
      name[definition_.path_.length()] = '\0';
      struct stat st;
      if (0 == ::stat(name, &st) && (file = ::fopen(name, "rb")) != nullptr) {
        file_length_ = static_cast<int64_t>(st.st_size);
        char *buffer = static_cast<char *>(allocator_.alloc(file_length_));
        if (buffer == nullptr) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (file_length_ != static_cast<int64_t>(::fread(buffer, 1, file_length_, file))) {
          ret = OB_IO_ERROR;
        } else {
          file_data_ = buffer;
        }
        (void) ::fclose(file);
      } else {
        ret = OB_FILE_NOT_EXIST;
      }
    }
    if (OB_SUCC(ret)) {
      // footer 位于文件末尾
      if (file_length_ < 12 ||
          0 != strncmp(reinterpret_cast<const char *>(file_data_ + file_length_ - 4), "PAR1", 4)) {
        ret = OB_INVALID_DATA;
      } else {
        const uint8_t *tail = reinterpret_cast<const uint8_t *>(file_data_ + file_length_ - 8);
        footer_length = static_cast<int64_t>(tail[0]) | (static_cast<int64_t>(tail[1]) << 8) |
                        (static_cast<int64_t>(tail[2]) << 16) | (static_cast<int64_t>(tail[3]) << 24);
        if (footer_length <= 0 || footer_length > file_length_ - 12) {
          ret = OB_INVALID_DATA;
        } else {
          footer = reinterpret_cast<const uint8_t *>(file_data_ + file_length_ - 8 - footer_length);
        }
      }
    }
    if (OB_SUCC(ret)) {
      CompactReader reader(footer, footer_length);
      int64_t num_rows = 0;
      OZ (parse_file_meta_data(reader, allocator_, schema_, row_groups_, num_rows));
    }
    if (OB_SUCC(ret)) {
      // 列数必须与绑定阶段推断的一致
      if (schema_.count() - 1 != definition_.columns_.count()) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA, "parquet schema does not match the bound columns");
      }
    }
    if (OB_SUCC(ret)) {
      OZ (load_group(0));
    }
    return ret;
  }
  int next_row(ObIArray<ObObj> &values) override
  {
    int ret = OB_SUCCESS;
    values.reuse();
    while (OB_SUCC(ret) && row_in_group_ >= group_rows_) {
      if (group_index_ + 1 >= row_groups_.count()) {
        ret = OB_ITER_END;
      } else if (OB_FAIL(load_group(group_index_ + 1))) {
      }
    }
    if (OB_SUCC(ret)) {
      const int64_t column_count = definition_.columns_.count();
      for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
        OZ (values.push_back(cells_[row_in_group_ * column_count + i]));
      }
      ++row_in_group_;
    }
    return ret;
  }
  int close() override
  {
    file_data_ = nullptr;
    file_length_ = 0;
    cells_ = nullptr;
    group_rows_ = 0;
    row_in_group_ = 0;
    group_index_ = -1;
    return OB_SUCCESS;
  }
private:
  int load_group(const int64_t index)
  {
    int ret = OB_SUCCESS;
    group_index_ = index;
    row_in_group_ = 0;
    group_rows_ = 0;
    cells_ = nullptr;
    if (index < 0 || index >= row_groups_.count()) {
      ret = OB_ITER_END;
    } else {
      const ParquetRowGroup &group = row_groups_.at(index);
      const int64_t column_count = definition_.columns_.count();
      group_rows_ = group.num_rows_;
      if (group_rows_ <= 0) {
        ret = OB_ITER_END;
      } else if (group.columns_.count() != column_count) {
        ret = OB_INVALID_DATA;
      } else {
        ObObj *cells = static_cast<ObObj *>(allocator_.alloc(
            static_cast<int64_t>(sizeof(ObObj)) * group_rows_ * column_count));
        if (cells == nullptr) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          for (int64_t i = 0; i < group_rows_ * column_count; ++i) {
            new (cells + i) ObObj();
            cells[i].set_null();
          }
          cells_ = cells;
          ObSEArray<ObObj, 64> column_values;
          for (int64_t c = 0; OB_SUCC(ret) && c < column_count; ++c) {
            column_values.reuse();
            OZ (decode_column(index, c, column_values));
            if (OB_SUCC(ret)) {
              for (int64_t r = 0; r < group_rows_; ++r) {
                cells[r * column_count + c] = r < column_values.count() ?
                                              column_values.at(r) : ObObj();
              }
            }
          }
        }
      }
    }
    return ret;
  }
  int decode_column(const int64_t group_index, const int64_t column_index,
                    ObIArray<ObObj> &values)
  {
    int ret = OB_SUCCESS;
    const ParquetRowGroup &group = row_groups_.at(group_index);
    const ParquetColumnChunk &chunk = group.columns_.at(column_index);
    const ParquetSchemaElement &element = schema_.at(column_index + 1);
    ObObjType type = ObNullType;
    int32_t length = 0;
    map_parquet_type(element, type, length);
    const int32_t max_def_level = element.repetition_ == 1 ? 1 : 0;
    int64_t pos = chunk.dictionary_page_offset_ >= 0 ? chunk.dictionary_page_offset_ :
                   chunk.data_page_offset_;
    if (pos <= 0 || pos >= file_length_) {
      ret = OB_INVALID_DATA;
    }
    ObSEArray<ObObj, 128> dictionary;
    int64_t total = 0;
    while (OB_SUCC(ret) && total < chunk.num_values_) {
      if (pos >= file_length_) {
        ret = OB_INVALID_DATA;
        break;
      }
      CompactReader header_reader(reinterpret_cast<const uint8_t *>(file_data_ + pos),
                                  file_length_ - pos);
      ParquetPageHeader header;
      OZ (parse_page_header(header_reader, header));
      const int64_t header_size = header_reader.pos();
      const int64_t page_offset = pos + header_size;
      if (OB_SUCC(ret) && (header.compressed_size_ < 0 ||
                           page_offset + header.compressed_size_ > file_length_)) {
        ret = OB_INVALID_DATA;
      }
      if (OB_SUCC(ret)) {
        const uint8_t *page_data = reinterpret_cast<const uint8_t *>(file_data_ + page_offset);
        pos = page_offset + header.compressed_size_;
        if (2 == header.type_) {
          // 字典页
          uint8_t *raw = static_cast<uint8_t *>(allocator_.alloc(header.uncompressed_size_ + 1));
          if (raw == nullptr) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(decompress_page(chunk.codec_, page_data, header.compressed_size_,
                                             raw, header.uncompressed_size_, allocator_))) {
          } else {
            int64_t value_pos = 0;
            for (int64_t i = 0; OB_SUCC(ret) && i < header.num_values_; ++i) {
              ObObj obj;
              OZ (decode_plain_value(element, type, raw, header.uncompressed_size_, value_pos,
                                     allocator_, obj));
              if (OB_SUCC(ret)) { OZ (dictionary.push_back(obj)); }
            }
          }
        } else if (0 == header.type_ || 3 == header.type_) {
          // 数据页（v1 / v2）
          uint8_t *raw = nullptr;
          const uint8_t *levels_data = nullptr;
          int64_t def_levels_length = 0;
          const uint8_t *values_data = nullptr;
          int64_t values_length = 0;
          if (0 == header.type_) {
            raw = static_cast<uint8_t *>(allocator_.alloc(header.uncompressed_size_ + 1));
            if (raw == nullptr) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(decompress_page(chunk.codec_, page_data, header.compressed_size_,
                                               raw, header.uncompressed_size_, allocator_))) {
            } else if (max_def_level > 0) {
              if (header.uncompressed_size_ < 4) {
                ret = OB_INVALID_DATA;
              } else {
                int32_t length_prefix = 0;
                MEMCPY(&length_prefix, raw, 4);
                def_levels_length = length_prefix;
                levels_data = raw + 4;
                values_data = raw + 4 + def_levels_length;
                values_length = header.uncompressed_size_ - 4 - def_levels_length;
              }
            } else {
              values_data = raw;
              values_length = header.uncompressed_size_;
            }
          } else {
            // v2：levels 不压缩
            def_levels_length = header.def_levels_length_;
            levels_data = page_data;
            const int64_t data_offset = header.def_levels_length_ + header.rep_levels_length_;
            if (data_offset > header.compressed_size_) {
              ret = OB_INVALID_DATA;
            } else if (header.is_compressed_) {
              // v2 的 uncompressed_page_size 包含未压缩的 rep/def levels
              const int64_t values_uncompressed = header.uncompressed_size_ - data_offset;
              const int64_t compressed_data_size = header.compressed_size_ - data_offset;
              if (values_uncompressed < 0 || compressed_data_size < 0) {
                ret = OB_INVALID_DATA;
              } else {
                raw = static_cast<uint8_t *>(allocator_.alloc(values_uncompressed + 1));
                if (raw == nullptr) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else if (OB_FAIL(decompress_page(chunk.codec_, page_data + data_offset,
                                                   compressed_data_size, raw,
                                                   values_uncompressed, allocator_))) {
                } else {
                  values_data = raw;
                  values_length = values_uncompressed;
                }
              }
            } else {
              values_data = page_data + data_offset;
              values_length = header.compressed_size_ - data_offset;
            }
          }
          // 定义层级
          ObSEArray<int64_t, 256> def_levels;
          if (OB_SUCC(ret) && max_def_level > 0 && def_levels_length > 0) {
            OZ (decode_rle_bitpacked(levels_data, def_levels_length, bit_width_of(max_def_level),
                                     header.num_values_, def_levels));
          }
          // 字典编码（PLAIN_DICTIONARY=2 / RLE_DICTIONARY=8）：首个字节是位宽，
          // 其后是 RLE/bit-packed 混合编码的下标流，只覆盖非空值。
          const bool dict_encoded = (2 == header.encoding_ || 8 == header.encoding_);
          ObSEArray<int64_t, 256> dict_indices;
          if (OB_SUCC(ret) && dict_encoded) {
            if (values_length < 1) {
              ret = OB_INVALID_DATA;
            } else {
              int64_t null_count = 0;
              for (int64_t i = 0; i < def_levels.count(); ++i) {
                if (def_levels.at(i) < max_def_level) { ++null_count; }
              }
              const int64_t non_null = header.num_values_ - null_count;
              const int32_t width = values_data[0];
              if (width < 0 || width > 32) {
                ret = OB_INVALID_DATA;
              } else if (OB_FAIL(decode_rle_bitpacked(values_data + 1, values_length - 1,
                                                      width, non_null, dict_indices))) {
              }
            }
          }
          int64_t value_pos = 0;
          int64_t index_pos = 0;
          for (int64_t i = 0; OB_SUCC(ret) && i < header.num_values_; ++i) {
            const bool is_null = max_def_level > 0 && i < def_levels.count() &&
                                 def_levels.at(i) < max_def_level;
            ObObj obj;
            obj.set_null();
            if (is_null) {
            } else if (dict_encoded) {
              if (index_pos >= dict_indices.count()) {
                ret = OB_INVALID_DATA;
              } else {
                const int64_t index = dict_indices.at(index_pos++);
                if (index < 0 || index >= dictionary.count()) {
                  ret = OB_INVALID_DATA;
                } else {
                  obj = dictionary.at(index);
                }
              }
            } else {
              OZ (decode_plain_value(element, type, values_data, values_length, value_pos,
                                     allocator_, obj));
            }
            if (OB_SUCC(ret)) { OZ (values.push_back(obj)); }
          }
          total += header.num_values_;
        } else {
          // 索引页等：跳过
        }
      }
    }
    if (OB_SUCC(ret) && values.count() < group.num_rows_) {
      // 定义层级提示行数不足时补齐 NULL
      while (OB_SUCC(ret) && values.count() < group.num_rows_) {
        ObObj obj;
        obj.set_null();
        OZ (values.push_back(obj));
      }
    }
    return ret;
  }
  const FileTableDefinition &definition_;
  ObIAllocator &allocator_;
  char *file_data_;
  int64_t file_length_;
  ObSEArray<ParquetSchemaElement, 64> schema_;
  ObSEArray<ParquetRowGroup, 8> row_groups_;
  int64_t group_index_;
  ObObj *cells_;
  int64_t group_rows_;
  int64_t row_in_group_;
};

} // namespace parquet_impl

using namespace parquet_impl;

int create_parquet_row_source(const FileTableDefinition &definition,
                              ObIAllocator &allocator,
                              FileTableRowSource *&source)
{
  int ret = OB_SUCCESS;
  source = nullptr;
  void *memory = allocator.alloc(sizeof(ParquetRowSource));
  if (memory == nullptr) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    source = new (memory) ParquetRowSource(definition, allocator);
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
