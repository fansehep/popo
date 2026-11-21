#pragma once

#include <memory>

#include "executor/vector/vector_arith.h"
#include "parser/ast.h"

namespace popo::executor
{

struct CompiledAgg
{
    popo::parser::AggFuncType func = popo::parser::AggFuncType::Count;
    bool is_star = false;
    bool is_int_result = false;
    int direct_col = -1;
    std::unique_ptr<VectorArith> arith;
};

} // namespace popo::executor
