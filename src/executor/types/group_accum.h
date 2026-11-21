#pragma once

#include <cstdint>

namespace popo::executor
{

struct GroupAccum
{
    double f64_val = 0.0;
    int64_t i64_val = 0;
    int64_t count = 0;
    bool init = false;
};

} // namespace popo::executor
