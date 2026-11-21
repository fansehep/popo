#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "executor/types/cached_subquery.h"

namespace popo::executor
{

struct CorrelatedSubqueryCache
{
    std::vector<std::pair<std::string, std::string>> outer_columns;
    std::unordered_map<std::string, CachedSubquery> entries;
};

} // namespace popo::executor
