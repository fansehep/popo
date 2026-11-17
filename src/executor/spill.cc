#include "executor/spill.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <queue>
#include <random>
#include <system_error>

namespace popo::executor
{
namespace
{

using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

popo::base::Result<void> io_error(std::string message)
{
    return popo::base::make_error(popo::base::Errno::IoErr, std::move(message));
}

template <typename T>
void append_scalar(std::vector<char> & out, T value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto old = out.size();
    out.resize(old + sizeof(T));
    std::memcpy(out.data() + old, &value, sizeof(T));
}

template <typename T>
bool read_scalar(std::span<const char> data, size_t & offset, T & value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > data.size() || data.size() - offset < sizeof(T))
        return false;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

popo::base::Result<std::vector<char>> encode_row(const Row & row)
{
    if (row.values_.size() > std::numeric_limits<uint32_t>::max())
        return popo::base::make_error(popo::base::Errno::InvalidArg, "spill row has too many columns");
    std::vector<char> data;
    data.reserve(estimate_row_bytes(row));
    append_scalar<uint32_t>(data, static_cast<uint32_t>(row.values_.size()));
    for (const Value & value : row.values_)
    {
        data.push_back(static_cast<char>(value.datatype()));
        switch (value.datatype())
        {
            case ValueType::Null:
                break;
            case ValueType::Uint8:
                append_scalar(data, value.u8());
                break;
            case ValueType::Uint16:
                append_scalar(data, value.u16());
                break;
            case ValueType::Uint32:
                append_scalar(data, value.u32());
                break;
            case ValueType::Uint64:
                append_scalar(data, value.u64());
                break;
            case ValueType::Int8:
                append_scalar(data, value.i8());
                break;
            case ValueType::Int16:
                append_scalar(data, value.i16());
                break;
            case ValueType::Int32:
                append_scalar(data, value.i32());
                break;
            case ValueType::Int64:
                append_scalar(data, value.i64());
                break;
            case ValueType::Float32:
                append_scalar(data, value.f32());
                break;
            case ValueType::Float64:
                append_scalar(data, value.f64());
                break;
            case ValueType::Bool:
                append_scalar<uint8_t>(data, value.boolean() ? 1 : 0);
                break;
            case ValueType::Char:
                append_scalar(data, value.u8());
                break;
            case ValueType::String: {
                if (value.string_ref().size() > std::numeric_limits<uint32_t>::max())
                    return popo::base::make_error(popo::base::Errno::InvalidArg, "spill string is too large");
                append_scalar<uint32_t>(data, static_cast<uint32_t>(value.string_ref().size()));
                data.insert(data.end(), value.string_ref().begin(), value.string_ref().end());
                break;
            }
        }
    }
    return data;
}

popo::base::Result<Row> decode_row(std::span<const char> data)
{
    size_t offset = 0;
    uint32_t count = 0;
    if (!read_scalar(data, offset, count))
        return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill row header");
    Row row;
    row.reserve_values(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (offset >= data.size())
            return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill value tag");
        const auto type = static_cast<ValueType>(static_cast<uint8_t>(data[offset++]));
        switch (type)
        {
            case ValueType::Null:
                row.append_value(Value());
                break;
#define POPO_DECODE_VALUE(kind, cpp_type) \
    case ValueType::kind: { \
        cpp_type value{}; \
        if (!read_scalar(data, offset, value)) \
            return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill value"); \
        row.append_value(Value(ValueType::kind, value)); \
        break; \
    }
                POPO_DECODE_VALUE(Uint8, uint8_t)
                POPO_DECODE_VALUE(Uint16, uint16_t)
                POPO_DECODE_VALUE(Uint32, uint32_t)
                POPO_DECODE_VALUE(Uint64, uint64_t)
                POPO_DECODE_VALUE(Int8, int8_t)
                POPO_DECODE_VALUE(Int16, int16_t)
                POPO_DECODE_VALUE(Int32, int32_t)
                POPO_DECODE_VALUE(Int64, int64_t)
                POPO_DECODE_VALUE(Float32, float)
                POPO_DECODE_VALUE(Float64, double)
#undef POPO_DECODE_VALUE
            case ValueType::Bool: {
                uint8_t value = 0;
                if (!read_scalar(data, offset, value))
                    return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill bool");
                row.append_value(Value(ValueType::Bool, value != 0));
                break;
            }
            case ValueType::Char: {
                uint8_t value = 0;
                if (!read_scalar(data, offset, value))
                    return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill char");
                row.append_value(Value(ValueType::Char, static_cast<char>(value)));
                break;
            }
            case ValueType::String: {
                uint32_t size = 0;
                if (!read_scalar(data, offset, size) || offset > data.size() || data.size() - offset < size)
                    return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill string");
                row.append_value(Value(ValueType::String, std::string(data.data() + offset, size)));
                offset += size;
                break;
            }
            default:
                return popo::base::make_error(popo::base::Errno::IoErr, "invalid spill value tag");
        }
    }
    if (offset != data.size())
        return popo::base::make_error(popo::base::Errno::IoErr, "spill row has trailing bytes");
    return row;
}

} // namespace

bool MemoryTracker::try_reserve(size_t bytes) noexcept
{
    size_t current = used_bytes_.load(std::memory_order_relaxed);
    for (;;)
    {
        if (bytes > limit_bytes_ || current > limit_bytes_ - bytes)
            return false;
        if (used_bytes_.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed))
        {
            size_t peak = peak_bytes_.load(std::memory_order_relaxed);
            while (peak < current + bytes
                   && !peak_bytes_.compare_exchange_weak(peak, current + bytes, std::memory_order_relaxed))
            {
            }
            return true;
        }
    }
}

void MemoryTracker::release(size_t bytes) noexcept
{
    used_bytes_.fetch_sub(std::min(bytes, used()), std::memory_order_relaxed);
}

MemoryReservation::MemoryReservation(MemoryTracker & tracker, size_t bytes)
{
    if (tracker.try_reserve(bytes))
    {
        tracker_ = &tracker;
        bytes_ = bytes;
    }
}

MemoryReservation::~MemoryReservation()
{
    reset();
}

MemoryReservation::MemoryReservation(MemoryReservation && other) noexcept
    : tracker_(std::exchange(other.tracker_, nullptr)), bytes_(std::exchange(other.bytes_, 0))
{
}

MemoryReservation & MemoryReservation::operator=(MemoryReservation && other) noexcept
{
    if (this != &other)
    {
        reset();
        tracker_ = std::exchange(other.tracker_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

void MemoryReservation::reset() noexcept
{
    if (tracker_)
        tracker_->release(bytes_);
    tracker_ = nullptr;
    bytes_ = 0;
}

TempFileManager::TempFileManager(std::filesystem::path root)
{
    if (root.empty())
    {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec);
        if (ec)
            return;
    }
    static std::atomic<uint64_t> next_id{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = root / std::format("popodb-query-{}-{}", stamp, next_id.fetch_add(1, std::memory_order_relaxed));
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec)
        directory_.clear();
}

TempFileManager::~TempFileManager()
{
    if (!directory_.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }
}

popo::base::Result<std::filesystem::path> TempFileManager::create(std::string_view purpose)
{
    if (directory_.empty())
        return popo::base::make_error(popo::base::Errno::IoErr, "could not create query temp directory");
    auto path = directory_ / std::format("{}-{}.spill", purpose, files_created_++);
    std::ofstream probe(path, std::ios::binary | std::ios::trunc);
    if (!probe)
        return popo::base::make_error(popo::base::Errno::IoErr, "could not create spill file: " + path.string());
    return path;
}

size_t estimate_value_bytes(const Value & value) noexcept
{
    return sizeof(Value) + (value.datatype() == ValueType::String ? value.string_ref().size() : 0);
}

size_t estimate_row_bytes(const Row & row) noexcept
{
    size_t bytes = sizeof(Row) + row.values_.capacity() * sizeof(Value);
    for (const auto & value : row.values_)
        if (value.datatype() == ValueType::String)
            bytes += value.string_ref().capacity();
    return bytes;
}

SpillWriter::SpillWriter(const std::filesystem::path & path, TempFileManager * owner)
    : out_(path, std::ios::binary | std::ios::trunc), owner_(owner)
{
}

popo::base::Result<void> SpillWriter::write(const Row & row)
{
    if (!out_)
        return io_error("spill output is not open");
    auto encoded = encode_row(row);
    if (!encoded)
        return std::unexpected(encoded.error());
    if (encoded->size() > std::numeric_limits<uint32_t>::max())
        return io_error("encoded spill row is too large");
    const uint32_t size = static_cast<uint32_t>(encoded->size());
    out_.write(reinterpret_cast<const char *>(&size), sizeof(size));
    out_.write(encoded->data(), static_cast<std::streamsize>(encoded->size()));
    if (!out_)
        return io_error("failed writing spill row");
    if (owner_)
        owner_->add_bytes_written(sizeof(size) + size);
    return {};
}

popo::base::Result<void> SpillWriter::close()
{
    if (!closed_)
    {
        out_.flush();
        closed_ = true;
        if (!out_)
            return io_error("failed flushing spill file");
        out_.close();
    }
    return {};
}

SpillReader::SpillReader(const std::filesystem::path & path) : in_(path, std::ios::binary)
{
}

popo::base::Result<std::optional<Row>> SpillReader::next()
{
    if (!in_)
    {
        if (in_.eof())
            return std::optional<Row>{};
        return popo::base::make_error(popo::base::Errno::IoErr, "spill input is not open");
    }
    uint32_t size = 0;
    in_.read(reinterpret_cast<char *>(&size), sizeof(size));
    if (in_.eof() && in_.gcount() == 0)
        return std::optional<Row>{};
    if (!in_)
        return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill record header");
    std::vector<char> data(size);
    in_.read(data.data(), static_cast<std::streamsize>(size));
    if (!in_)
        return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill record");
    auto decoded = decode_row(data);
    if (!decoded)
        return std::unexpected(decoded.error());
    return std::optional<Row>(std::move(*decoded));
}

popo::base::Result<std::vector<Row>> external_sort(
    const std::function<popo::base::Result<std::optional<Row>>()> & input,
    const RowLess & less,
    MemoryTracker & memory,
    TempFileManager & temp)
{
    {
        std::vector<std::filesystem::path> runs;
        std::vector<Row> chunk;
        size_t charged = 0;
        auto flush = [&]() -> popo::base::Result<void>
        {
            if (chunk.empty())
                return {};
            std::stable_sort(chunk.begin(), chunk.end(), less);
            auto path = temp.create("sort-run");
            if (!path)
                return std::unexpected(path.error());
            SpillWriter writer(*path, &temp);
            for (const auto & row : chunk)
            {
                auto written = writer.write(row);
                if (!written)
                    return written;
            }
            auto closed = writer.close();
            if (!closed)
                return closed;
            runs.push_back(*path);
            memory.release(charged);
            charged = 0;
            chunk.clear();
            return {};
        };

        for (;;)
        {
            auto item = input();
            if (!item)
                return std::unexpected(item.error());
            if (!*item)
                break;
            const size_t bytes = estimate_row_bytes(**item);
            if (!memory.try_reserve(bytes))
            {
                auto flushed = flush();
                if (!flushed)
                    return std::unexpected(flushed.error());
                if (!memory.try_reserve(bytes))
                    return popo::base::make_error(
                        popo::base::Errno::InvalidArg, "one sort row exceeds the query memory budget");
            }
            charged += bytes;
            chunk.push_back(std::move(**item));
        }

        if (runs.empty())
        {
            std::stable_sort(chunk.begin(), chunk.end(), less);
            memory.release(charged);
            return chunk;
        }
        auto flushed = flush();
        if (!flushed)
            return std::unexpected(flushed.error());

        struct Head
        {
            Row row;
            size_t run;
        };
        struct Greater
        {
            const RowLess * less;
            bool operator()(const Head & a, const Head & b) const
            {
                if ((*less)(b.row, a.row))
                    return true;
                if ((*less)(a.row, b.row))
                    return false;
                // Runs are produced in input order and stable-sorted. For equal
                // keys, draining the lower run first preserves global stability.
                return a.run > b.run;
            }
        };
        std::vector<std::unique_ptr<SpillReader>> readers;
        std::priority_queue<Head, std::vector<Head>, Greater> heap(Greater{&less});
        for (size_t i = 0; i < runs.size(); ++i)
        {
            readers.push_back(std::make_unique<SpillReader>(runs[i]));
            auto row = readers.back()->next();
            if (!row)
                return std::unexpected(row.error());
            if (*row)
                heap.push(Head{std::move(**row), i});
        }
        std::vector<Row> output;
        while (!heap.empty())
        {
            Head head = std::move(const_cast<Head &>(heap.top()));
            heap.pop();
            output.push_back(std::move(head.row));
            auto row = readers[head.run]->next();
            if (!row)
                return std::unexpected(row.error());
            if (*row)
                heap.push(Head{std::move(**row), head.run});
        }
        return output;
    }
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
    {
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
}

} // namespace popo::executor
