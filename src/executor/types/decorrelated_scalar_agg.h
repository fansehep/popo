#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "executor/vector/flat_int64_map.h"
#include "executor/vector/flat_pair64_map.h"
#include "parser/ast.h"
#include "type/value.h"

namespace popo::executor
{

struct DecorrelatedScalarAgg
{
    bool eligible = false;
    std::vector<popo::parser::ExprPtr> outer_key_exprs;
    bool keys_are_i64 = false;
    FlatInt64Map<popo::type::Value> map_1i64;
    FlatPair64Map<popo::type::Value> map_2i64;
    std::unordered_map<std::string, popo::type::Value> map_str;
    popo::type::Value empty_default;
};

} // namespace popo::executor
