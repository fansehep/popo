#pragma once

#include <cstdint>
#include <set>
#include <string>

#include "type/value.h"

namespace popo::executor
{

struct AggState
{
    int64_t count = 0;
    popo::type::Value result;
    bool initialized = false;
    std::set<std::string> distinct_seen;
    std::string last_distinct;
    bool has_last_distinct = false;
};

} // namespace popo::executor
