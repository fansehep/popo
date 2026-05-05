#include "src/storage/redo_log.h"
#include <cstring>
#include <filesystem>

namespace neon::storage
{

// ============================================================================
// LogRecord serialization
// ============================================================================

// Format:
// [uint32_t total_size] [lsn_t lsn] [txn_id_t txn_id] [uint8_t type]
// [uint16_t table_name_len] [char[] table_name]
// [page_id_t page_id] [slot_id_t slot_id]
// [uint32_t old_data_len] [char[] old_data]
// [uint32_t new_data_len] [char[] new_data]
// [uint32_t total_size]  // repeated at end for backward scanning

uint32_t LogRecord::serialized_size() const
{
    uint32_t size = 0;
    size += sizeof(uint32_t); // total_size header
    size += sizeof(lsn_t); // lsn
    size += sizeof(txn_id_t); // txn_id
    size += sizeof(uint8_t); // type
    size += sizeof(uint16_t); // table_name_len
    size += static_cast<uint32_t>(table_name.size());
    size += sizeof(page_id_t); // page_id
    size += sizeof(slot_id_t); // slot_id
    size += sizeof(uint32_t); // old_data_len
    size += static_cast<uint32_t>(old_data.size());
    size += sizeof(uint32_t); // new_data_len
    size += static_cast<uint32_t>(new_data.size());
    size += sizeof(uint32_t); // total_size trailer
    return size;
}

std::vector<char> LogRecord::serialize() const
{
    uint32_t total = serialized_size();
    std::vector<char> buf(total);
    char * ptr = buf.data();

    auto write = [&](const void * src, size_t n)
    {
        std::memcpy(ptr, src, n);
        ptr += n;
    };

    write(&total, sizeof(uint32_t));
    write(&lsn, sizeof(lsn_t));
    write(&txn_id, sizeof(txn_id_t));
    auto t = static_cast<uint8_t>(type);
    write(&t, sizeof(uint8_t));

    auto name_len = static_cast<uint16_t>(table_name.size());
    write(&name_len, sizeof(uint16_t));
    if (name_len > 0)
    {
        write(table_name.data(), name_len);
    }

    write(&page_id, sizeof(page_id_t));
    write(&slot_id, sizeof(slot_id_t));

    auto old_len = static_cast<uint32_t>(old_data.size());
    write(&old_len, sizeof(uint32_t));
    if (old_len > 0)
    {
        write(old_data.data(), old_len);
    }

    auto new_len = static_cast<uint32_t>(new_data.size());
    write(&new_len, sizeof(uint32_t));
    if (new_len > 0)
    {
        write(new_data.data(), new_len);
    }

    write(&total, sizeof(uint32_t)); // trailer

    return buf;
}

LogRecord LogRecord::deserialize(const char * data, uint32_t len)
{
    LogRecord record;
    const char * ptr = data;

    auto read = [&](void * dst, size_t n)
    {
        std::memcpy(dst, ptr, n);
        ptr += n;
    };

    uint32_t total_size = 0;
    read(&total_size, sizeof(uint32_t));
    read(&record.lsn, sizeof(lsn_t));
    read(&record.txn_id, sizeof(txn_id_t));

    uint8_t t = 0;
    read(&t, sizeof(uint8_t));
    record.type = static_cast<LogType>(t);

    uint16_t name_len = 0;
    read(&name_len, sizeof(uint16_t));
    if (name_len > 0)
    {
        record.table_name.assign(ptr, name_len);
        ptr += name_len;
    }

    read(&record.page_id, sizeof(page_id_t));
    read(&record.slot_id, sizeof(slot_id_t));

    uint32_t old_len = 0;
    read(&old_len, sizeof(uint32_t));
    if (old_len > 0)
    {
        record.old_data.assign(ptr, ptr + old_len);
        ptr += old_len;
    }

    uint32_t new_len = 0;
    read(&new_len, sizeof(uint32_t));
    if (new_len > 0)
    {
        record.new_data.assign(ptr, ptr + new_len);
        ptr += new_len;
    }

    // Skip trailing total_size
    return record;
}

// ============================================================================
// RedoLogManager
// ============================================================================

RedoLogManager::RedoLogManager(const std::string & log_file_path) : log_file_path_(log_file_path)
{
    // If log file already exists, scan for max LSN
    if (std::filesystem::exists(log_file_path_))
    {
        auto records = read_all_records();
        lsn_t max_lsn = 0;
        for (const auto & rec : records)
        {
            if (rec.lsn > max_lsn)
            {
                max_lsn = rec.lsn;
            }
        }
        if (max_lsn > 0)
        {
            next_lsn_ = max_lsn + 1;
            flushed_lsn_ = max_lsn;
        }
    }
    open_log_file();
}

RedoLogManager::~RedoLogManager()
{
    if (!log_buffer_.empty())
    {
        flush();
    }
    if (log_file_.is_open())
    {
        log_file_.close();
    }
}

void RedoLogManager::open_log_file()
{
    log_file_.open(log_file_path_, std::ios::binary | std::ios::app);
}

lsn_t RedoLogManager::append(const LogRecord & record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    LogRecord rec = record;
    rec.lsn = next_lsn_++;
    auto serialized = rec.serialize();
    log_buffer_.insert(log_buffer_.end(), serialized.begin(), serialized.end());
    return rec.lsn;
}

void RedoLogManager::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_buffer_.empty())
    {
        return;
    }
    log_file_.write(log_buffer_.data(), static_cast<std::streamsize>(log_buffer_.size()));
    log_file_.flush();
    // The highest LSN in the buffer is next_lsn_ - 1
    flushed_lsn_ = next_lsn_ - 1;
    log_buffer_.clear();
}

void RedoLogManager::flush_to_lsn(lsn_t target_lsn)
{
    // Only need to check outside lock; flush() acquires its own lock
    if (target_lsn > flushed_lsn_)
    {
        flush();
    }
}

std::vector<LogRecord> RedoLogManager::read_all_records()
{
    std::vector<LogRecord> records;

    std::ifstream in(log_file_path_, std::ios::binary);
    if (!in.is_open())
    {
        return records;
    }

    // Read entire file into memory
    in.seekg(0, std::ios::end);
    auto file_size = in.tellg();
    if (file_size <= 0)
    {
        return records;
    }
    in.seekg(0, std::ios::beg);

    std::vector<char> file_data(static_cast<size_t>(file_size));
    in.read(file_data.data(), file_size);
    in.close();

    const char * ptr = file_data.data();
    const char * end = ptr + file_data.size();

    while (ptr + sizeof(uint32_t) <= end)
    {
        uint32_t total_size = 0;
        std::memcpy(&total_size, ptr, sizeof(uint32_t));

        if (total_size == 0 || ptr + total_size > end)
        {
            break; // Corrupted or incomplete record
        }

        auto record = LogRecord::deserialize(ptr, total_size);
        records.push_back(std::move(record));
        ptr += total_size;
    }

    return records;
}

void RedoLogManager::truncate()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_.is_open())
    {
        log_file_.close();
    }
    // Reopen in truncate mode
    log_file_.open(log_file_path_, std::ios::binary | std::ios::trunc);
    log_file_.close();
    // Reopen in append mode for future writes
    log_file_.open(log_file_path_, std::ios::binary | std::ios::app);
    next_lsn_ = 1;
    flushed_lsn_ = 0;
    log_buffer_.clear();
}

} // namespace neon::storage
