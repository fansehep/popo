#include "executor/spill/spill_algorithms.h"

#include <format>
#include <memory>

#include "executor/spill/spill_writer.h"

namespace popo::executor
{

using popo::type::Row;

popo::base::Result<std::vector<Row>> external_sort(
    const std::function<popo::base::Result<std::optional<Row>>()> & input,
    const RowLess & less,
    MemoryTracker & memory,
    TempFileManager & temp)
{
    ExternalSorter sorter(less, memory, temp);
    for (;;)
    {
        auto item = input();
        if (!item)
            return std::unexpected(item.error());
        if (!*item)
            break;
        auto pushed = sorter.push(std::move(**item));
        if (!pushed)
            return std::unexpected(pushed.error());
    }
    return sorter.finish();
}

popo::base::Result<std::vector<std::filesystem::path>> partition_rows(
    const std::function<popo::base::Result<std::optional<Row>>()> & input,
    const std::function<size_t(const Row &)> & hash,
    size_t partitions,
    TempFileManager & temp,
    std::string_view purpose)
{
    if (partitions < 2)
        return popo::base::make_error(popo::base::Errno::InvalidArg, "spill partition count must be at least two");
    std::vector<std::filesystem::path> paths;
    std::vector<std::unique_ptr<SpillWriter>> writers;
    paths.reserve(partitions);
    writers.reserve(partitions);
    for (size_t i = 0; i < partitions; ++i)
    {
        auto path = temp.create(std::format("{}-{}", purpose, i));
        if (!path)
            return std::unexpected(path.error());
        paths.push_back(*path);
        writers.push_back(std::make_unique<SpillWriter>(*path, &temp));
    }
    for (;;)
    {
        auto row = input();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            break;
        auto written = writers[hash(**row) % partitions]->write(**row);
        if (!written)
            return std::unexpected(written.error());
    }
    for (auto & writer : writers)
    {
        auto closed = writer->close();
        if (!closed)
            return std::unexpected(closed.error());
    }
    return paths;
}

} // namespace popo::executor
