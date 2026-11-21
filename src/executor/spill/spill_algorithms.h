#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "executor/spill/external_sorter.h"
#include "executor/spill/memory_tracker.h"
#include "executor/spill/temp_file_manager.h"
#include "type/tuple.h"

namespace popo::executor
{

// External stable merge sort. The input callback returns nullopt at EOF.
popo::base::Result<std::vector<popo::type::Row>> external_sort(
    const std::function<popo::base::Result<std::optional<popo::type::Row>>()> & input,
    const RowLess & less,
    MemoryTracker & memory,
    TempFileManager & temp);

// Hash partition helper used by joins and aggregations. The caller chooses a
// fresh seed when recursively repartitioning a skewed partition.
popo::base::Result<std::vector<std::filesystem::path>> partition_rows(
    const std::function<popo::base::Result<std::optional<popo::type::Row>>()> & input,
    const std::function<size_t(const popo::type::Row &)> & hash,
    size_t partitions,
    TempFileManager & temp,
    std::string_view purpose);

} // namespace popo::executor
