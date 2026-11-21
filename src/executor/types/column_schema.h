#pragma once

#include <string>
#include <vector>

namespace popo::executor
{

struct ColumnSchema
{
    std::string table;
    std::string alias;
    std::vector<std::string> columns;
};

} // namespace popo::executor
