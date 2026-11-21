#pragma once

#include <cstdint>

namespace popo::executor
{

struct DistinctTwoI64
{
    int64_t first = 0;
    uint8_t count = 0; // 0, 1, or 2 (meaning >= 2 distinct values)
};

} // namespace popo::executor
