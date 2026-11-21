#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "catalog/schema.h"
#include "executor/vector/aligned_buffer.h"
#include "executor/vector/column_vector.h"
#include "executor/vector/vector_constants.h"
#include "type/tuple.h"
#include "type/value.h"

namespace popo::executor
{

// Columnar in-memory batch of up to kVectorBatchSize rows with 64-byte aligned buffers.
struct ColumnarBatch
{
    size_t count = 0;
    std::vector<ColumnVector> columns;
    AlignedBuffer<uint8_t> mask;  // 0xFF = active, 0x00 = filtered out
    AlignedBuffer<uint16_t> sel;  // Compact selection vector of passing row indices
    size_t sel_count = 0;
    bool has_sel = false;

    void init(std::span<const popo::type::ValueType> types, size_t capacity = kVectorBatchSize);
    void reset();

    // Decode selected physical columns directly from serialized tuple bytes into columnar arrays.
    void append_serialized_tuple(
        const char * data, uint32_t len,
        const popo::catalog::Schema & schema,
        std::span<const uint16_t> phys_cols);

    // Append an already-materialized Row (MVCC overlay/history fallback).
    void append_row(const popo::type::Row & row);

    // Materialize a single passing row back into Row.
    void extract_row(size_t row_idx, popo::type::Row & out) const;
    popo::type::Row extract_row(size_t row_idx) const
    {
        popo::type::Row out;
        extract_row(row_idx, out);
        return out;
    }
};

} // namespace popo::executor
