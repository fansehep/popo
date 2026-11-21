#pragma once

#include <cstddef>

namespace popo::executor
{

struct EquiEdgeInfo
{
    size_t leaf_a = 0;
    size_t leaf_b = 0;
    size_t eq_root = 0;
    double ndv = 100.0;
    bool a_is_pk = false;
    bool b_is_pk = false;
};

} // namespace popo::executor
