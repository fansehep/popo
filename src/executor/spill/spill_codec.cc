#include "executor/spill/spill_codec.h"

#include <cstring>
#include <limits>
#include <string>
#include <type_traits>

namespace popo::executor
{

using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

namespace
{

template <typename T>
void append_scalar(std::vector<char> & out, T value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto old = out.size();
    out.resize(old + sizeof(T));
    std::memcpy(out.data() + old, &value, sizeof(T));
}

template <typename T>
bool read_scalar(std::span<const char> data, size_t & offset, T & value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > data.size() || data.size() - offset < sizeof(T))
        return false;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

} // namespace

size_t estimate_value_bytes(const Value & value) noexcept
{
    return sizeof(Value) + (value.datatype() == ValueType::String ? value.string_ref().size() : 0);
}

size_t estimate_row_bytes(const Row & row) noexcept
{
    size_t bytes = sizeof(Row) + row.values_.capacity() * sizeof(Value);
    for (const auto & value : row.values_)
        if (value.datatype() == ValueType::String)
            bytes += value.string_ref().capacity();
    return bytes;
}

popo::base::Result<std::vector<char>> encode_spill_row(const Row & row)
{
    if (row.values_.size() > std::numeric_limits<uint32_t>::max())
        return popo::base::make_error(popo::base::Errno::InvalidArg, "spill row has too many columns");
    std::vector<char> data;
    data.reserve(estimate_row_bytes(row));
    append_scalar<uint32_t>(data, static_cast<uint32_t>(row.values_.size()));
    for (const Value & value : row.values_)
    {
        data.push_back(static_cast<char>(value.datatype()));
        switch (value.datatype())
        {
            case ValueType::Null:
                break;
            case ValueType::Uint8:
                append_scalar(data, value.u8());
                break;
            case ValueType::Uint16:
                append_scalar(data, value.u16());
                break;
            case ValueType::Uint32:
                append_scalar(data, value.u32());
                break;
            case ValueType::Uint64:
                append_scalar(data, value.u64());
                break;
            case ValueType::Int8:
                append_scalar(data, value.i8());
                break;
            case ValueType::Int16:
                append_scalar(data, value.i16());
                break;
            case ValueType::Int32:
                append_scalar(data, value.i32());
                break;
            case ValueType::Int64:
                append_scalar(data, value.i64());
                break;
            case ValueType::Float32:
                append_scalar(data, value.f32());
                break;
            case ValueType::Float64:
                append_scalar(data, value.f64());
                break;
            case ValueType::Bool:
                append_scalar<uint8_t>(data, value.boolean() ? 1 : 0);
                break;
            case ValueType::Char:
                append_scalar(data, value.u8());
                break;
            case ValueType::String: {
                if (value.string_ref().size() > std::numeric_limits<uint32_t>::max())
                    return popo::base::make_error(popo::base::Errno::InvalidArg, "spill string is too large");
                append_scalar<uint32_t>(data, static_cast<uint32_t>(value.string_ref().size()));
                data.insert(data.end(), value.string_ref().begin(), value.string_ref().end());
                break;
            }
        }
    }
    return data;
}

popo::base::Result<Row> decode_spill_row(std::span<const char> data)
{
    size_t offset = 0;
    uint32_t count = 0;
    if (!read_scalar(data, offset, count))
        return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill row header");
    Row row;
    row.reserve_values(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (offset >= data.size())
            return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill value tag");
        const auto type = static_cast<ValueType>(static_cast<uint8_t>(data[offset++]));
        switch (type)
        {
            case ValueType::Null:
                row.append_value(Value());
                break;
#define POPO_DECODE_VALUE(kind, cpp_type) \
    case ValueType::kind: { \
        cpp_type value{}; \
        if (!read_scalar(data, offset, value)) \
            return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill value"); \
        row.append_value(Value(ValueType::kind, value)); \
        break; \
    }
                POPO_DECODE_VALUE(Uint8, uint8_t)
                POPO_DECODE_VALUE(Uint16, uint16_t)
                POPO_DECODE_VALUE(Uint32, uint32_t)
                POPO_DECODE_VALUE(Uint64, uint64_t)
                POPO_DECODE_VALUE(Int8, int8_t)
                POPO_DECODE_VALUE(Int16, int16_t)
                POPO_DECODE_VALUE(Int32, int32_t)
                POPO_DECODE_VALUE(Int64, int64_t)
                POPO_DECODE_VALUE(Float32, float)
                POPO_DECODE_VALUE(Float64, double)
#undef POPO_DECODE_VALUE
            case ValueType::Bool: {
                uint8_t value = 0;
                if (!read_scalar(data, offset, value))
                    return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill bool");
                row.append_value(Value(ValueType::Bool, value != 0));
                break;
            }
            case ValueType::Char: {
                uint8_t value = 0;
                if (!read_scalar(data, offset, value))
                    return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill char");
                row.append_value(Value(ValueType::Char, static_cast<char>(value)));
                break;
            }
            case ValueType::String: {
                uint32_t size = 0;
                if (!read_scalar(data, offset, size) || offset > data.size() || data.size() - offset < size)
                    return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill string");
                row.append_value(Value(ValueType::String, std::string(data.data() + offset, size)));
                offset += size;
                break;
            }
            default:
                return popo::base::make_error(popo::base::Errno::IoErr, "invalid spill value tag");
        }
    }
    if (offset != data.size())
        return popo::base::make_error(popo::base::Errno::IoErr, "spill row has trailing bytes");
    return row;
}

} // namespace popo::executor
