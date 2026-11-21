#pragma once

#include <string>
#include <unordered_set>

#include "executor/types/distinct_two_i64.h"
#include "executor/vector/flat_int64_map.h"
#include "executor/vector/flat_int64_set.h"
#include "parser/ast.h"

namespace popo::executor
{

struct DecorrelatedExists
{
    bool eligible = false;
    bool has_ne_residual = false;
    popo::parser::ExprPtr outer_eq_expr;
    popo::parser::ExprPtr outer_ne_expr;
    bool key_is_i64 = false;
    FlatInt64Set i64_keys;
    std::unordered_set<std::string> str_keys;
    FlatInt64Map<DistinctTwoI64> i64_ne_map;
};

} // namespace popo::executor
