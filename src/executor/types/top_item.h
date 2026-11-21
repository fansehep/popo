#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "parser/ast.h"
#include "type/tuple.h"
#include "type/value.h"

namespace popo::executor
{

struct TopItem
{
    std::vector<popo::type::Value> keys;
    popo::type::Row row;
    std::unordered_map<const popo::parser::Expr *, popo::type::Value> aggs;
    uint64_t seq = 0;
};

} // namespace popo::executor
