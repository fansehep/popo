#include "src/storage/redo_log.h"
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

namespace popo::storage
{

// ============================================================================
// LogRecord serialization
// ============================================================================
//
// Layout (little endian, native):
//   [u32 total_size]
//   [lsn][prev_lsn][txn_id][u8 type][u8 compensation]
//   [u16 name_len][name bytes]
//   [page_id][slot_id][undo_next_lsn]
//   [u32 old_len][old bytes][u32 new_len][new bytes]
//   [u32 checkpoint_count]{[txn_id][last_lsn][u8 status]}...
//   [u32 total_size]   // trailer for backward scanning

uint32_t LogRecord::serialized_size() const
{
    uint32_t size = 0;
    size += sizeof(uint32_t); // total_size header
    size += sizeof(lsn_t); // lsn
    size += sizeof(lsn_t); // prev_lsn
    size += sizeof(txn_id_t); // txn_id
    size += sizeof(uint8_t); // type
    size += sizeof(uint8_t); // compensation
    size += sizeof(uint16_t); // name_len
    size += static_cast<uint32_t>(table_name.size());
    size += sizeof(page_id_t);
    size += sizeof(slot_id_t);
    size += sizeof(lsn_t); // undo_next_lsn
    size += sizeof(uint32_t) + static_cast<uint32_t>(old_data.size());
    size += sizeof(uint32_t) + static_cast<uint32_t>(new_data.size());
    size += sizeof(uint32_t); // checkpoint count
    size += static_cast<uint32_t>(checkpoint_txns.size()) * (sizeof(txn_id_t) + sizeof(lsn_t) + sizeof(uint8_t));
    size += sizeof(uint32_t); // trailer
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
    write(&prev_lsn, sizeof(lsn_t));
    write(&txn_id, sizeof(txn_id_t));
    auto t = static_cast<uint8_t>(type);
    write(&t, sizeof(uint8_t));
    auto comp = static_cast<uint8_t>(compensation);
    write(&comp, sizeof(uint8_t));

    auto name_len = static_cast<uint16_t>(table_name.size());
    write(&name_len, sizeof(uint16_t));
    if (name_len > 0)
    {
        write(table_name.data(), name_len);
    }

    write(&page_id, sizeof(page_id_t));
    write(&slot_id, sizeof(slot_id_t));
    write(&undo_next_lsn, sizeof(lsn_t));

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

    auto ckpt_count = static_cast<uint32_t>(checkpoint_txns.size());
    write(&ckpt_count, sizeof(uint32_t));
    for (const auto & a : checkpoint_txns)
    {
        write(&a.txn_id, sizeof(txn_id_t));
        write(&a.last_lsn, sizeof(lsn_t));
        write(&a.status, sizeof(uint8_t));
    }

    write(&total, sizeof(uint32_t)); // trailer
    return buf;
}

LogRecord LogRecord::deserialize(const char * data, uint32_t len)
{
    LogRecord record;
    const char * ptr = data;
    const char * end = data + len;

    auto read = [&](void * dst, size_t n)
    {
        std::memcpy(dst, ptr, n);
        ptr += n;
    };

    uint32_t total_size = 0;
    read(&total_size, sizeof(uint32_t));
    read(&record.lsn, sizeof(lsn_t));
    read(&record.prev_lsn, sizeof(lsn_t));
    read(&record.txn_id, sizeof(txn_id_t));

    uint8_t t = 0;
    read(&t, sizeof(uint8_t));
    record.type = static_cast<LogType>(t);
    uint8_t comp = 0;
    read(&comp, sizeof(uint8_t));
    record.compensation = static_cast<LogType>(comp);

    uint16_t name_len = 0;
    read(&name_len, sizeof(uint16_t));
    if (name_len > 0)
    {
        record.table_name.assign(ptr, name_len);
        ptr += name_len;
    }

    read(&record.page_id, sizeof(page_id_t));
    read(&record.slot_id, sizeof(slot_id_t));
    read(&record.undo_next_lsn, sizeof(lsn_t));

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

    uint32_t ckpt_count = 0;
    if (ptr + sizeof(uint32_t) <= end)
    {
        read(&ckpt_count, sizeof(uint32_t));
        for (uint32_t i = 0; i < ckpt_count && ptr + sizeof(txn_id_t) + sizeof(lsn_t) + sizeof(uint8_t) <= end; ++i)
        {
            ActiveTxn a;
            read(&a.txn_id, sizeof(txn_id_t));
            read(&a.last_lsn, sizeof(lsn_t));
            read(&a.status, sizeof(uint8_t));
            record.checkpoint_txns.push_back(a);
        }
    }

    return record;
}

// ============================================================================
// RedoLogManager
// ============================================================================

RedoLogManager::RedoLogManager(const std::string & log_file_path) : log_file_path_(log_file_path)
{
    // The watermark file keeps LSNs monotonic even after the log is truncated
    // by a checkpoint, so pageLSN comparisons stay valid across restarts.
    std::ifstream meta(log_file_path_ + ".lsn", std::ios::binary);
    if (meta.is_open())
    {
        lsn_t watermark = 0;
        meta.read(reinterpret_cast<char *>(&watermark), sizeof(watermark));
        if (meta.gcount() == sizeof(watermark) && watermark > 0)
        {
            next_lsn_ = watermark;
            flushed_lsn_ = watermark - 1;
        }
    }
    std::error_code ec;
    if (std::filesystem::exists(log_file_path_, ec))
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
        if (max_lsn >= next_lsn_)
        {
            next_lsn_ = max_lsn + 1;
            flushed_lsn_ = max_lsn;
        }
    }
    open_log_file();
}

RedoLogManager::~RedoLogManager()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!log_buffer_.empty())
    {
        flush_locked();
    }
    if (log_file_.is_open())
    {
        log_file_.close();
    }
    if (log_fd_ >= 0)
    {
        ::close(log_fd_);
        log_fd_ = -1;
    }
}

void RedoLogManager::open_log_file()
{
    log_file_.open(log_file_path_, std::ios::binary | std::ios::app);
    if (log_fd_ >= 0)
    {
        ::close(log_fd_);
    }
    // ofstream::flush only drains userspace buffers, so a second descriptor on
    // the same inode is what lets flush_locked() issue a durability barrier.
    log_fd_ = ::open(log_file_path_.c_str(), O_WRONLY);
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

void RedoLogManager::flush_locked()
{
    if (log_buffer_.empty())
    {
        return;
    }
    log_file_.write(log_buffer_.data(), static_cast<std::streamsize>(log_buffer_.size()));
    log_file_.flush();
    if (log_fd_ >= 0)
    {
        ::fdatasync(log_fd_);
    }
    flushed_lsn_ = next_lsn_ - 1;
    log_buffer_.clear();
    flush_cv_.notify_all();
}

void RedoLogManager::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    flush_locked();
}

void RedoLogManager::flush_to_lsn(lsn_t target_lsn)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (target_lsn <= flushed_lsn_)
    {
        return;
    }
    // Group commit: the first caller flushes the shared buffer; concurrent
    // callers whose target is already covered wait for it instead of issuing
    // a second fsync.
    flush_locked();
    flush_cv_.wait(lock, [&] { return flushed_lsn_ >= target_lsn; });
}

std::vector<LogRecord> RedoLogManager::read_all_records()
{
    std::vector<LogRecord> records;

    std::ifstream in(log_file_path_, std::ios::binary);
    if (!in.is_open())
    {
        return records;
    }

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

        if (total_size < sizeof(uint32_t) * 2 || ptr + total_size > end)
        {
            break; // Corrupted or torn tail record
        }

        auto record = LogRecord::deserialize(ptr, total_size);
        records.push_back(std::move(record));
        ptr += total_size;
    }

    return records;
}

void RedoLogManager::persist_watermark()
{
    const std::string meta_path = log_file_path_ + ".lsn";
    int fd = ::open(meta_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        return;
    }
    lsn_t watermark = next_lsn_;
    ssize_t written = ::write(fd, &watermark, sizeof(watermark));
    (void)written;
    ::fsync(fd);
    ::close(fd);
}

void RedoLogManager::truncate()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!log_buffer_.empty())
    {
        flush_locked();
    }
    // Persist the next LSN before dropping the log so numbering stays
    // monotonic across the checkpoint and any later restart.
    persist_watermark();
    if (log_file_.is_open())
    {
        log_file_.close();
    }
    log_file_.open(log_file_path_, std::ios::binary | std::ios::trunc);
    log_file_.close();
    open_log_file();
    log_buffer_.clear();
    flushed_lsn_ = next_lsn_ - 1;
}

} // namespace popo::storage