#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>

#include "executor/types/batch.h"

namespace popo::executor
{

struct JoinInput
{
    Batch batch;
    std::optional<std::filesystem::path> spill_path;
    size_t charged = 0;
};

} // namespace popo::executor
