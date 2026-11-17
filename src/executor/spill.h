#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "base/error.h"
#include "type/tuple.h"

namespace popo::executor
{

// Query-scoped accounting. Operators reserve only resident data; spill files are
// deliberately not charged to the memory budget.
class MemoryTracker
{
public:
    explicit MemoryTracker(size_t limit_bytes) : limit_bytes_(limit_bytes) { }

    bool try_reserve(size_t bytes) noexcept;
    void release(size_t bytes) noexcept;
    size_t used() const noexcept { return used_bytes_.load(std::memory_order_relaxed); }
    size_t peak() const noexcept { return peak_bytes_.load(std::memory_order_relaxed); }
    size_t limit() const noexcept { return limit_bytes_; }

private:
    size_t limit_bytes_;
    std::atomic<size_t> used_bytes_{0};
    std::atomic<size_t> peak_bytes_{0};
};

class MemoryReservation
{
public:
    MemoryReservation() = default;
    MemoryReservation(MemoryTracker & tracker, size_t bytes);
    ~MemoryReservation();
    MemoryReservation(MemoryReservation && other) noexcept;
    MemoryReservation & operator=(MemoryReservation && other) noexcept;
    MemoryReservation(const MemoryReservation &) = delete;
    MemoryReservation & operator=(const MemoryReservation &) = delete;

    bool acquired() const noexcept { return tracker_ != nullptr; }
    size_t bytes() const noexcept { return bytes_; }
    void reset() noexcept;

private:
    MemoryTracker * tracker_ = nullptr;
    size_t bytes_ = 0;
};

// Owns every file created by one statement. Destruction is the final cleanup
// boundary on success, expected errors, and exceptions from the C++ runtime.
class TempFileManager
{
public:
    explicit TempFileManager(std::filesystem::path root = {});
    ~TempFileManager();
    TempFileManager(const TempFileManager &) = delete;
    TempFileManager & operator=(const TempFileManager &) = delete;

    popo::base::Result<std::filesystem::path> create(std::string_view purpose);
    const std::filesystem::path & directory() const noexcept { return directory_; }
    size_t files_created() const noexcept { return files_created_; }
    uint64_t bytes_written() const noexcept { return bytes_written_; }
    void add_bytes_written(uint64_t bytes) noexcept { bytes_written_ += bytes; }

private:
    std::filesystem::path directory_;
    size_t files_created_ = 0;
    uint64_t bytes_written_ = 0;
};

size_t estimate_value_bytes(const popo::type::Value & value) noexcept;
size_t estimate_row_bytes(const popo::type::Row & row) noexcept;

// Stable, typed binary encoding. Records are length-framed and contain their
// ValueType tags, so runs and partitions do not depend on an in-memory schema.
class SpillWriter
{
public:
    SpillWriter(const std::filesystem::path & path, TempFileManager * owner = nullptr);
    popo::base::Result<void> write(const popo::type::Row & row);
    popo::base::Result<void> close();

private:
    std::ofstream out_;
    TempFileManager * owner_;
    bool closed_ = false;
};

class SpillReader
{
public:
    explicit SpillReader(const std::filesystem::path & path);
    // nullopt is clean EOF.
    popo::base::Result<std::optional<popo::type::Row>> next();

private:
    std::ifstream in_;
};

using RowLess = std::function<bool(const popo::type::Row &, const popo::type::Row &)>;

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
