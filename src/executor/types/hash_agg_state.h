#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

#include "type/value.h"

namespace popo::executor
{

struct HashAggState
{
    int64_t count = 0;
    int64_t sum_i64 = 0;
    double sum_f64 = 0.0;
    bool is_float_sum = false;
    popo::type::Value val_result;
    bool initialized = false;
    std::unordered_set<std::string> distinct_seen;
};

} // namespace popo::executor
