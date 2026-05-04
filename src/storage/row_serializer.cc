#include "src/storage/row_serializer.h"
#include <cstring>
#include <cmath>
#include <stdexcept>

namespace neon::storage {

using neon::type::ValueType;
using neon::type::Value;
using neon::type::Row;
using neon::catalog::Schema;

// Helper: size of a fixed-type value in bytes
static uint32_t fixed_type_size(ValueType type) {
    switch (type) {
        case ValueType::Bool:    return 1;
        case ValueType::Int8:
        case ValueType::Uint8:
        case ValueType::Char:    return 1;
        case ValueType::Int16:
        case ValueType::Uint16:  return 2;
        case ValueType::Int32:
        case ValueType::Uint32:
        case ValueType::Float32: return 4;
        case ValueType::Int64:
        case ValueType::Uint64:
        case ValueType::Float64: return 8;
        default:                 return 0;  // variable-length or null
    }
}

uint32_t RowSerializer::serialized_size(const Row& row, const Schema& schema) {
    uint16_t num_cols = static_cast<uint16_t>(schema.columns_.size());
    // Header: num_cols (2 bytes) + null bitmap
    uint32_t null_bitmap_bytes = (num_cols + 7) / 8;
    uint32_t size = sizeof(uint16_t) + null_bitmap_bytes;

    for (uint16_t i = 0; i < num_cols; ++i) {
        if (i < row.values_.size() && row.values_[i].datatype() != ValueType::Null) {
            ValueType type = schema.columns_[i].data_type_;
            if (type == ValueType::String) {
                size += sizeof(uint16_t) + static_cast<uint32_t>(row.values_[i].string_ref().size());
            } else {
                size += fixed_type_size(type);
            }
        }
        // Null columns take no data space (only bitmap bit)
    }
    return size;
}

std::vector<char> RowSerializer::serialize(const Row& row, const Schema& schema) {
    uint16_t num_cols = static_cast<uint16_t>(schema.columns_.size());
    uint32_t null_bitmap_bytes = (num_cols + 7) / 8;

    uint32_t total_size = serialized_size(row, schema);
    std::vector<char> buf(total_size, 0);
    uint32_t pos = 0;

    // Write num_cols
    std::memcpy(buf.data() + pos, &num_cols, sizeof(uint16_t));
    pos += sizeof(uint16_t);

    // Write null bitmap
    uint32_t bitmap_start = pos;
    pos += null_bitmap_bytes;  // Skip bitmap area, fill it as we go

    for (uint16_t i = 0; i < num_cols; ++i) {
        bool is_null = (i >= row.values_.size()) || (row.values_[i].datatype() == ValueType::Null);
        if (is_null) {
            // Set bit i in null bitmap
            uint32_t byte_idx = i / 8;
            uint8_t bit_idx = i % 8;
            buf[bitmap_start + byte_idx] |= static_cast<char>(1 << bit_idx);
            continue;
        }

        const Value& val = row.values_[i];
        ValueType type = schema.columns_[i].data_type_;

        switch (type) {
            case ValueType::Bool: {
                uint8_t v = val.boolean() ? 1 : 0;
                std::memcpy(buf.data() + pos, &v, 1);
                pos += 1;
                break;
            }
            case ValueType::Int8: {
                int8_t v = val.i8();
                std::memcpy(buf.data() + pos, &v, 1);
                pos += 1;
                break;
            }
            case ValueType::Uint8: {
                uint8_t v = val.u8();
                std::memcpy(buf.data() + pos, &v, 1);
                pos += 1;
                break;
            }
            case ValueType::Char: {
                uint8_t v = val.u8();
                std::memcpy(buf.data() + pos, &v, 1);
                pos += 1;
                break;
            }
            case ValueType::Int16: {
                int16_t v = val.i16();
                std::memcpy(buf.data() + pos, &v, 2);
                pos += 2;
                break;
            }
            case ValueType::Uint16: {
                uint16_t v = val.u16();
                std::memcpy(buf.data() + pos, &v, 2);
                pos += 2;
                break;
            }
            case ValueType::Int32: {
                int32_t v = val.i32();
                std::memcpy(buf.data() + pos, &v, 4);
                pos += 4;
                break;
            }
            case ValueType::Uint32: {
                uint32_t v = val.u32();
                std::memcpy(buf.data() + pos, &v, 4);
                pos += 4;
                break;
            }
            case ValueType::Float32: {
                float v = val.f32();
                std::memcpy(buf.data() + pos, &v, 4);
                pos += 4;
                break;
            }
            case ValueType::Int64: {
                int64_t v = val.i64();
                std::memcpy(buf.data() + pos, &v, 8);
                pos += 8;
                break;
            }
            case ValueType::Uint64: {
                uint64_t v = val.u64();
                std::memcpy(buf.data() + pos, &v, 8);
                pos += 8;
                break;
            }
            case ValueType::Float64: {
                double v = val.f64();
                std::memcpy(buf.data() + pos, &v, 8);
                pos += 8;
                break;
            }
            case ValueType::String: {
                const std::string& s = val.string_ref();
                uint16_t str_len = static_cast<uint16_t>(s.size());
                std::memcpy(buf.data() + pos, &str_len, sizeof(uint16_t));
                pos += sizeof(uint16_t);
                if (str_len > 0) {
                    std::memcpy(buf.data() + pos, s.data(), str_len);
                    pos += str_len;
                }
                break;
            }
            default:
                break;
        }
    }
    return buf;
}

Row RowSerializer::deserialize(const char* data, uint32_t len, const Schema& schema) {
    Row row;
    uint32_t pos = 0;

    if (len < sizeof(uint16_t)) {
        return row;
    }

    // Read num_cols
    uint16_t num_cols = 0;
    std::memcpy(&num_cols, data + pos, sizeof(uint16_t));
    pos += sizeof(uint16_t);

    uint32_t null_bitmap_bytes = (num_cols + 7) / 8;
    if (pos + null_bitmap_bytes > len) {
        return row;
    }

    const char* bitmap = data + pos;
    pos += null_bitmap_bytes;

    for (uint16_t i = 0; i < num_cols && i < schema.columns_.size(); ++i) {
        // Check null bitmap
        uint32_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;
        bool is_null = (bitmap[byte_idx] >> bit_idx) & 1;

        if (is_null) {
            row.append_value(Value());  // Null value
            continue;
        }

        ValueType type = schema.columns_[i].data_type_;

        switch (type) {
            case ValueType::Bool: {
                uint8_t v = 0;
                std::memcpy(&v, data + pos, 1);
                pos += 1;
                row.append_value(Value(ValueType::Bool, static_cast<bool>(v)));
                break;
            }
            case ValueType::Int8: {
                int8_t v = 0;
                std::memcpy(&v, data + pos, 1);
                pos += 1;
                row.append_value(Value(ValueType::Int8, v));
                break;
            }
            case ValueType::Uint8: {
                uint8_t v = 0;
                std::memcpy(&v, data + pos, 1);
                pos += 1;
                row.append_value(Value(ValueType::Uint8, v));
                break;
            }
            case ValueType::Char: {
                uint8_t v = 0;
                std::memcpy(&v, data + pos, 1);
                pos += 1;
                row.append_value(Value(ValueType::Char, static_cast<char>(v)));
                break;
            }
            case ValueType::Int16: {
                int16_t v = 0;
                std::memcpy(&v, data + pos, 2);
                pos += 2;
                row.append_value(Value(ValueType::Int16, v));
                break;
            }
            case ValueType::Uint16: {
                uint16_t v = 0;
                std::memcpy(&v, data + pos, 2);
                pos += 2;
                row.append_value(Value(ValueType::Uint16, v));
                break;
            }
            case ValueType::Int32: {
                int32_t v = 0;
                std::memcpy(&v, data + pos, 4);
                pos += 4;
                row.append_value(Value(ValueType::Int32, v));
                break;
            }
            case ValueType::Uint32: {
                uint32_t v = 0;
                std::memcpy(&v, data + pos, 4);
                pos += 4;
                row.append_value(Value(ValueType::Uint32, v));
                break;
            }
            case ValueType::Float32: {
                float v = 0;
                std::memcpy(&v, data + pos, 4);
                pos += 4;
                row.append_value(Value(ValueType::Float32, v));
                break;
            }
            case ValueType::Int64: {
                int64_t v = 0;
                std::memcpy(&v, data + pos, 8);
                pos += 8;
                row.append_value(Value(ValueType::Int64, v));
                break;
            }
            case ValueType::Uint64: {
                uint64_t v = 0;
                std::memcpy(&v, data + pos, 8);
                pos += 8;
                row.append_value(Value(ValueType::Uint64, v));
                break;
            }
            case ValueType::Float64: {
                double v = 0;
                std::memcpy(&v, data + pos, 8);
                pos += 8;
                row.append_value(Value(ValueType::Float64, v));
                break;
            }
            case ValueType::String: {
                uint16_t str_len = 0;
                std::memcpy(&str_len, data + pos, sizeof(uint16_t));
                pos += sizeof(uint16_t);
                std::string s(data + pos, str_len);
                pos += str_len;
                row.append_value(Value(ValueType::String, s));
                break;
            }
            default:
                row.append_value(Value());  // Unknown type -> null
                break;
        }
    }
    return row;
}

}  // namespace neon::storage
