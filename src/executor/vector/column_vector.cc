#include "executor/vector/column_vector.h"

#include <string>

#include "function/date_functions.h"

namespace popo::executor
{

using popo::type::Value;
using popo::type::ValueType;

void ColumnVector::init(ValueType vt, size_t capacity)
{
    logical_type = vt;
    kind = col_kind_for(vt);
    nulls.resize(capacity);
    has_nulls = false;
    all_len1 = true;
    all_len10 = true;
    switch (kind)
    {
        case ColKind::Int64:
            i64_data.resize(capacity);
            break;
        case ColKind::Float64:
            f64_data.resize(capacity);
            break;
        case ColKind::Bool:
            u8_data.resize(capacity);
            break;
        case ColKind::String:
            u8_data.resize(capacity);
            str_offsets.resize(capacity + 1);
            str_offsets[0] = 0;
            str10_hi.resize(capacity);
            str10_lo.resize(capacity);
            str_bytes.clear();
            str_bytes.reserve(capacity * 16);
            break;
    }
}

void ColumnVector::reset()
{
    has_nulls = false;
    all_len1 = true;
    all_len10 = true;
    if (kind == ColKind::String)
    {
        str_bytes.clear();
        if (str_offsets.size() > 0)
            str_offsets[0] = 0;
    }
}

void ColumnVector::append_null(size_t row) noexcept
{
    nulls[row] = 1;
    has_nulls = true;
    switch (kind)
    {
        case ColKind::Int64:
            i64_data[row] = 0;
            break;
        case ColKind::Float64:
            f64_data[row] = 0.0;
            break;
        case ColKind::Bool:
            u8_data[row] = 0;
            break;
        case ColKind::String:
            u8_data[row] = 0;
            str10_hi[row] = 0;
            str10_lo[row] = 0;
            str_offsets[row + 1] = str_offsets[row];
            all_len1 = false;
            all_len10 = false;
            break;
    }
}

void ColumnVector::append_i64(size_t row, int64_t v) noexcept
{
    nulls[row] = 0;
    i64_data[row] = v;
}

void ColumnVector::append_f64(size_t row, double v) noexcept
{
    nulls[row] = 0;
    f64_data[row] = v;
}

void ColumnVector::append_bool(size_t row, bool v) noexcept
{
    nulls[row] = 0;
    u8_data[row] = v ? 1 : 0;
}

void ColumnVector::append_string(size_t row, const char * ptr, uint32_t len)
{
    nulls[row] = 0;
    const size_t start = str_bytes.size();
    if (len > 0)
        str_bytes.insert(str_bytes.end(), ptr, ptr + len);
    str_offsets[row + 1] = static_cast<uint32_t>(start + len);
    if (len == 1)
    {
        u8_data[row] = static_cast<uint8_t>(ptr[0]);
        all_len10 = false;
    }
    else if (len == 10)
    {
        uint64_t hi = 0;
        uint16_t lo = 0;
        popo::function::pack_str10(ptr, hi, lo);
        str10_hi[row] = hi;
        str10_lo[row] = lo;
        all_len1 = false;
    }
    else
    {
        all_len1 = false;
        all_len10 = false;
    }
}

void ColumnVector::append_value(size_t row, const Value & v)
{
    if (v.datatype() == ValueType::Null)
    {
        append_null(row);
        return;
    }
    switch (kind)
    {
        case ColKind::Int64: {
            int64_t iv = 0;
            switch (v.datatype())
            {
                case ValueType::Int8: iv = v.i8(); break;
                case ValueType::Int16: iv = v.i16(); break;
                case ValueType::Int32: iv = v.i32(); break;
                case ValueType::Int64: iv = v.i64(); break;
                case ValueType::Uint8: iv = v.u8(); break;
                case ValueType::Uint16: iv = v.u16(); break;
                case ValueType::Uint32: iv = v.u32(); break;
                case ValueType::Uint64: iv = static_cast<int64_t>(v.u64()); break;
                case ValueType::Char: iv = v.u8(); break;
                case ValueType::Bool: iv = v.boolean() ? 1 : 0; break;
                default: iv = static_cast<int64_t>(v.to_f64()); break;
            }
            append_i64(row, iv);
            break;
        }
        case ColKind::Float64:
            append_f64(row, v.to_f64());
            break;
        case ColKind::Bool:
            append_bool(row, v.datatype() == ValueType::Bool ? v.boolean() : (v.to_f64() != 0.0));
            break;
        case ColKind::String: {
            if (v.datatype() == ValueType::String)
            {
                const auto & s = v.string_ref();
                append_string(row, s.data(), static_cast<uint32_t>(s.size()));
            }
            else
            {
                std::string s = v.to_val_string();
                append_string(row, s.data(), static_cast<uint32_t>(s.size()));
            }
            break;
        }
    }
}

Value ColumnVector::to_value(size_t row) const
{
    if (has_nulls && nulls[row])
        return Value();
    switch (logical_type)
    {
        case ValueType::Bool: return Value(ValueType::Bool, u8_data[row] != 0);
        case ValueType::Int8: return Value(ValueType::Int8, static_cast<int8_t>(i64_data[row]));
        case ValueType::Uint8: return Value(ValueType::Uint8, static_cast<uint8_t>(i64_data[row]));
        case ValueType::Char: return Value(ValueType::Char, static_cast<char>(i64_data[row]));
        case ValueType::Int16: return Value(ValueType::Int16, static_cast<int16_t>(i64_data[row]));
        case ValueType::Uint16: return Value(ValueType::Uint16, static_cast<uint16_t>(i64_data[row]));
        case ValueType::Int32: return Value(ValueType::Int32, static_cast<int32_t>(i64_data[row]));
        case ValueType::Uint32: return Value(ValueType::Uint32, static_cast<uint32_t>(i64_data[row]));
        case ValueType::Int64: return Value(ValueType::Int64, i64_data[row]);
        case ValueType::Uint64: return Value(ValueType::Uint64, static_cast<uint64_t>(i64_data[row]));
        case ValueType::Float32: return Value(ValueType::Float32, static_cast<float>(f64_data[row]));
        case ValueType::Float64: return Value(ValueType::Float64, f64_data[row]);
        case ValueType::String: {
            const uint32_t begin = str_offsets[row];
            const uint32_t end = str_offsets[row + 1];
            return Value(ValueType::String, std::string(str_bytes.data() + begin, end - begin));
        }
        default:
            return Value();
    }
}

} // namespace popo::executor
