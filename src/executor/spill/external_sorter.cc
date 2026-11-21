#include "executor/spill/external_sorter.h"

#include <algorithm>
#include <memory>
#include <queue>
#include <utility>

#include "executor/spill/spill_codec.h"
#include "executor/spill/spill_reader.h"
#include "executor/spill/spill_writer.h"

namespace popo::executor
{

using popo::type::Row;

ExternalSorter::ExternalSorter(RowLess less, MemoryTracker & memory, TempFileManager & temp)
    : less_(std::move(less)), memory_(memory), temp_(temp)
{
}

ExternalSorter::~ExternalSorter()
{
    if (charged_ > 0)
    {
        memory_.release(charged_);
        charged_ = 0;
    }
}

popo::base::Result<void> ExternalSorter::flush_chunk()
{
    if (chunk_.empty())
        return {};
    std::stable_sort(chunk_.begin(), chunk_.end(), less_);
    auto path = temp_.create("sort-run");
    if (!path)
        return std::unexpected(path.error());
    SpillWriter writer(*path, &temp_);
    for (const auto & row : chunk_)
    {
        auto written = writer.write(row);
        if (!written)
            return written;
    }
    auto closed = writer.close();
    if (!closed)
        return closed;
    runs_.push_back(*path);
    memory_.release(charged_);
    charged_ = 0;
    chunk_.clear();
    return {};
}

popo::base::Result<void> ExternalSorter::push(Row row)
{
    const size_t bytes = estimate_row_bytes(row);
    if (!memory_.try_reserve(bytes))
    {
        auto flushed = flush_chunk();
        if (!flushed)
            return flushed;
        if (!memory_.try_reserve(bytes))
            return popo::base::make_error(
                popo::base::Errno::InvalidArg, "one sort row exceeds the query memory budget");
    }
    charged_ += bytes;
    chunk_.push_back(std::move(row));
    return {};
}

popo::base::Result<void> ExternalSorter::for_each(const std::function<popo::base::Result<bool>(Row &&)> & consumer)
{
    if (runs_.empty())
    {
        std::stable_sort(chunk_.begin(), chunk_.end(), less_);
        std::vector<Row> local = std::move(chunk_);
        memory_.release(charged_);
        charged_ = 0;
        for (auto & row : local)
        {
            auto cont = consumer(std::move(row));
            if (!cont)
                return std::unexpected(cont.error());
            if (!*cont)
                break;
        }
        return {};
    }

    auto flushed = flush_chunk();
    if (!flushed)
        return flushed;

    using MergeEntry = std::pair<Row, size_t>;
    auto greater = [&](const MergeEntry & a, const MergeEntry & b)
    {
        if (less_(b.first, a.first))
            return true;
        if (less_(a.first, b.first))
            return false;
        return a.second > b.second;
    };

    std::vector<std::unique_ptr<SpillReader>> readers;
    readers.reserve(runs_.size());
    std::priority_queue<MergeEntry, std::vector<MergeEntry>, decltype(greater)> heap(greater);
    for (size_t i = 0; i < runs_.size(); ++i)
    {
        readers.push_back(std::make_unique<SpillReader>(runs_[i]));
        auto row = readers.back()->next();
        if (!row)
            return std::unexpected(row.error());
        if (*row)
            heap.emplace(std::move(**row), i);
    }
    while (!heap.empty())
    {
        MergeEntry head = std::move(const_cast<MergeEntry &>(heap.top()));
        heap.pop();
        const size_t run_idx = head.second;
        auto cont = consumer(std::move(head.first));
        if (!cont)
            return std::unexpected(cont.error());
        if (!*cont)
            break;
        auto row = readers[run_idx]->next();
        if (!row)
            return std::unexpected(row.error());
        if (*row)
            heap.emplace(std::move(**row), run_idx);
    }
    return {};
}

popo::base::Result<std::vector<Row>> ExternalSorter::finish()
{
    if (runs_.empty())
    {
        std::stable_sort(chunk_.begin(), chunk_.end(), less_);
        std::vector<Row> output = std::move(chunk_);
        memory_.release(charged_);
        charged_ = 0;
        return output;
    }
    std::vector<Row> output;
    auto done = for_each(
        [&](Row && row) -> popo::base::Result<bool>
        {
            output.push_back(std::move(row));
            return true;
        });
    if (!done)
        return std::unexpected(done.error());
    return output;
}

} // namespace popo::executor
