#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "executor/types/batch.h"
#include "type/tuple.h"

namespace popo::executor
{

struct StreamMaterialized
{
    Batch layout;
    std::vector<popo::type::Row> in_memory_rows;
    std::optional<std::filesystem::path> path;
};

} // namespace popo::executor
