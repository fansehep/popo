#pragma once

#include <cstddef>
#include <cstdint>

#include "type/value.h"

namespace popo::executor
{

inline constexpr size_t kVectorBatchSize = 2048;
inline constexpr size_t kSimdAlignment = 64;

enum class ColKind : uint8_t
{
    Int64,
    Float64,
    Bool,
    String,
};

inline ColKind col_kind_for(popo::type::ValueType vt) noexcept
{
    switch (vt)
    {
        case popo::type::ValueType::Float32:
        case popo::type::ValueType::Float64:
            return ColKind::Float64;
        case popo::type::ValueType::Bool:
            return ColKind::Bool;
        case popo::type::ValueType::String:
            return ColKind::String;
        default:
            return ColKind::Int64;
    }
}

} // namespace popo::executor
