#pragma once

#include <cstddef>
#include <string>

#include "parser/ast.h"

namespace popo::executor
{

struct ColNode
{
    size_t leaf = 0;
    std::string col;
    popo::parser::ExprPtr expr;
};

} // namespace popo::executor
