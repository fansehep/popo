#pragma once

#include <unordered_map>

#include "executor/types/batch.h"
#include "parser/ast.h"
#include "type/tuple.h"
#include "type/value.h"

namespace popo::executor
{

struct EvalEnv
{
    const Batch * layout = nullptr;
    const popo::type::Row * row = nullptr;
    const EvalEnv * outer = nullptr;
    const std::unordered_map<const popo::parser::Expr *, popo::type::Value> * aggs = nullptr;
    int depth = 0;
};

} // namespace popo::executor
