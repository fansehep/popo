#include "executor/vector/columnar_batch.h"

#include <algorithm>
#include <cstring>

namespace popo::executor
{

using popo::type::Row;
using popo::type::ValueType;

namespace
{

uint32_t fixed_type_size(ValueType type) noexcept
{
    switch (type)
    {
        case ValueType::Bool:
        case ValueType::Int8:
        case ValueType::Uint8:
        case ValueType::Char:
            return 1;
        case ValueType::Int16:
        case ValueType::Uint16:
            return 2;
        case ValueType::Int32:
        case ValueType::Uint32:
        case ValueType::Float32:
            return 4;
        case ValueType::Int64:
        case ValueType::Uint64:
        case ValueType::Float64:
            return 8;
        default:
            return 0;
    }
}

} // namespace

void ColumnarBatch::init(std::span<const ValueType> types, size_t capacity)
{
    count = 0;
    sel_count = 0;
    has_sel = false;
    mask.resize(capacity);
    sel.resize(capacity);
    columns.resize(types.size());
    for (size_t i = 0; i < types.size(); ++i)
        columns[i].init(types[i], capacity);
}

void ColumnarBatch::reset()
{
    count = 0;
    sel_count = 0;
    has_sel = false;
    for (auto & col : columns)
        col.reset();
}

void ColumnarBatch::append_serialized_tuple(
    const char * data, uint32_t len,
    const popo::catalog::Schema & schema,
    std::span<const uint16_t> phys_cols)
{
    const size_t row_idx = count++;
    if (data == nullptr || len < sizeof(uint16_t))
    {
        for (auto & col : columns)
            col.append_null(row_idx);
        return;
    }

    uint16_t num_cols = 0;
    std::memcpy(&num_cols, data, sizeof(uint16_t));
    uint32_t pos = sizeof(uint16_t);
    const uint32_t null_bitmap_bytes = (num_cols + 7) / 8;
    const char * bitmap = data + pos;
    pos += null_bitmap_bytes;

    size_t pick = 0;
    const size_t n_pick = phys_cols.size();
    const uint16_t schema_cols = static_cast<uint16_t>(schema.columns_.size());
    const uint16_t limit = std::min(num_cols, schema_cols);

    for (uint16_t i = 0; i < limit && pick < n_pick; ++i)
    {
        const bool want = (phys_cols[pick] == i);
        const bool is_null = ((bitmap[i >> 3] >> (i & 7)) & 1) != 0;
        const ValueType type = schema.columns_[i].data_type_;

        if (is_null)
        {
            if (want)
            {
                columns[pick].append_null(row_idx);
                ++pick;
            }
            continue;
        }

        if (!want)
        {
            if (type == ValueType::String)
            {
                uint16_t str_len = 0;
                std::memcpy(&str_len, data + pos, sizeof(uint16_t));
                pos += sizeof(uint16_t) + str_len;
            }
            else
            {
                pos += fixed_type_size(type);
            }
            continue;
        }

        auto & col = columns[pick];
        switch (type)
        {
            case ValueType::Bool: {
                uint8_t v = 0;
                std::memcpy(&v, data + pos, 1);
                pos += 1;
                col.append_bool(row_idx, v != 0);
                break;
            }
            case ValueType::Int8: {
                int8_t v = 0;
                std::memcpy(&v, data + pos, 1);
                pos += 1;
                col.append_i64(row_idx, v);
                break;
            }
            case ValueType::Uint8:
            case ValueType::Char: {
                uint8_t v = 0;
                std::memcpy(&v, data + pos, 1);
                pos += 1;
                col.append_i64(row_idx, v);
                break;
            }
            case ValueType::Int16: {
                int16_t v = 0;
                std::memcpy(&v, data + pos, 2);
                pos += 2;
                col.append_i64(row_idx, v);
                break;
            }
            case ValueType::Uint16: {
                uint16_t v = 0;
                std::memcpy(&v, data + pos, 2);
                pos += 2;
                col.append_i64(row_idx, v);
                break;
            }
            case ValueType::Int32: {
                int32_t v = 0;
                std::memcpy(&v, data + pos, 4);
                pos += 4;
                col.append_i64(row_idx, v);
                break;
            }
            case ValueType::Uint32: {
                uint32_t v = 0;
                std::memcpy(&v, data + pos, 4);
                pos += 4;
                col.append_i64(row_idx, v);
                break;
            }
            case ValueType::Float32: {
                float v = 0;
                std::memcpy(&v, data + pos, 4);
                pos += 4;
                col.append_f64(row_idx, static_cast<double>(v));
                break;
            }
            case ValueType::Int64: {
                int64_t v = 0;
                std::memcpy(&v, data + pos, 8);
                pos += 8;
                col.append_i64(row_idx, v);
                break;
            }
            case ValueType::Uint64: {
                uint64_t v = 0;
                std::memcpy(&v, data + pos, 8);
                pos += 8;
                col.append_i64(row_idx, static_cast<int64_t>(v));
                break;
            }
            case ValueType::Float64: {
                double v = 0;
                std::memcpy(&v, data + pos, 8);
                pos += 8;
                col.append_f64(row_idx, v);
                break;
            }
            case ValueType::String: {
                uint16_t str_len = 0;
                std::memcpy(&str_len, data + pos, sizeof(uint16_t));
                pos += sizeof(uint16_t);
                col.append_string(row_idx, data + pos, str_len);
                pos += str_len;
                break;
            }
            default:
                col.append_null(row_idx);
                break;
        }
        ++pick;
    }

    while (pick < n_pick)
    {
        columns[pick].append_null(row_idx);
        ++pick;
    }
}

void ColumnarBatch::append_row(const Row & row)
{
    const size_t row_idx = count++;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (i < row.values_.size())
            columns[i].append_value(row_idx, row.values_[i]);
        else
            columns[i].append_null(row_idx);
    }
}

void ColumnarBatch::extract_row(size_t row_idx, Row & out) const
{
    out.values_.clear();
    out.values_.reserve(columns.size());
    for (const auto & col : columns)
        out.values_.push_back(col.to_value(row_idx));
}

} // namespace popo::executor
