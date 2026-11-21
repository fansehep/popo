#pragma once

#include <cstddef>
#include <set>

#include "parser/ast.h"

namespace popo::executor
{

struct JoinPred
{
    popo::parser::ExprPtr expr;
    std::set<size_t> deps;
    bool is_equi = false;
    bool placed = false;
};

} // namespace popo::executor
