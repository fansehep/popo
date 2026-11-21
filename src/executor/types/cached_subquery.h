#pragma once

#include <string>
#include <unordered_set>

#include "executor/types/batch.h"
#include "executor/vector/flat_int64_set.h"

namespace popo::executor
{

struct CachedSubquery
{
    Batch batch;
    std::unordered_set<std::string> first_col_keys;
    FlatInt64Set first_col_i64;
    bool first_col_is_i64 = false;
    bool has_null = false;
    bool indexed = false;
};

} // namespace popo::executor
