#include "executor/types/batch.h"

namespace popo::executor
{

size_t Batch::width() const
{
    size_t n = 0;
    for (const auto & s : schemas)
        n += s.columns.size();
    return n;
}

std::vector<std::string> Batch::flat_names() const
{
    std::vector<std::string> out;
    for (const auto & s : schemas)
        for (const auto & c : s.columns)
            out.push_back(c);
    return out;
}

} // namespace popo::executor
