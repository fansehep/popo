#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <vector>

#include "base/error.h"
#include "executor/spill/memory_tracker.h"
#include "executor/spill/temp_file_manager.h"
#include "type/tuple.h"

namespace popo::executor
{

using RowLess = std::function<bool(const popo::type::Row &, const popo::type::Row &)>;

// Push-based external stable merge sorter. Keeps rows in memory and sorts in
// place when they fit within MemoryTracker; spills sorted runs only on overflow.
class ExternalSorter
{
public:
    ExternalSorter(RowLess less, MemoryTracker & memory, TempFileManager & temp);
    ~ExternalSorter();
    ExternalSorter(const ExternalSorter &) = delete;
    ExternalSorter & operator=(const ExternalSorter &) = delete;

    popo::base::Result<void> push(popo::type::Row row);
    popo::base::Result<void> for_each(const std::function<popo::base::Result<bool>(popo::type::Row &&)> & consumer);
    popo::base::Result<std::vector<popo::type::Row>> finish();

private:
    popo::base::Result<void> flush_chunk();

    RowLess less_;
    MemoryTracker & memory_;
    TempFileManager & temp_;
    std::vector<std::filesystem::path> runs_;
    std::vector<popo::type::Row> chunk_;
    size_t charged_ = 0;
};

} // namespace popo::executor
