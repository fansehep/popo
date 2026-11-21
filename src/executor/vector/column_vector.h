#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "executor/vector/aligned_buffer.h"
#include "executor/vector/vector_constants.h"
#include "type/value.h"

namespace popo::executor
{

// Columnar storage for a single column across up to kVectorBatchSize rows.
struct ColumnVector
{
    ColKind kind = ColKind::Int64;
    popo::type::ValueType logical_type = popo::type::ValueType::Int64;

    AlignedBuffer<int64_t> i64_data;
    AlignedBuffer<double> f64_data;
    AlignedBuffer<uint8_t> u8_data; // Bool data OR 1-byte string fast lane
    AlignedBuffer<uint8_t> nulls;   // 0 = valid, 1 = NULL

    // Contiguous string arena (zero per-row std::string heap allocations).
    std::vector<char> str_bytes;
    AlignedBuffer<uint32_t> str_offsets; // size kVectorBatchSize + 1
    // Fast SIMD lanes for fixed-length 10-byte strings (ISO DATE 'YYYY-MM-DD').
    AlignedBuffer<uint64_t> str10_hi;
    AlignedBuffer<uint16_t> str10_lo;

    bool has_nulls = false;
    bool all_len1 = true;
    bool all_len10 = true;

    void init(popo::type::ValueType vt, size_t capacity = kVectorBatchSize);
    void reset();

    std::string_view str_at(size_t row) const noexcept
    {
        const uint32_t begin = str_offsets[row];
        const uint32_t end = str_offsets[row + 1];
        return std::string_view(str_bytes.data() + begin, end - begin);
    }

    void append_null(size_t row) noexcept;
    void append_i64(size_t row, int64_t v) noexcept;
    void append_f64(size_t row, double v) noexcept;
    void append_bool(size_t row, bool v) noexcept;
    void append_string(size_t row, const char * ptr, uint32_t len);
    void append_value(size_t row, const popo::type::Value & v);
    popo::type::Value to_value(size_t row) const;
};

} // namespace popo::executor
