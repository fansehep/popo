#pragma once

#include <unordered_map>

#include "parser/ast.h"
#include "type/tuple.h"
#include "type/value.h"

namespace popo::executor
{

struct StreamItem
{
    popo::type::Row row;
    std::unordered_map<const popo::parser::Expr *, popo::type::Value> aggs;
};

} // namespace popo::executor
