#pragma once
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "src/storage/common.h"

namespace popo::storage
{

enum class LogType : uint8_t
{
    INVALID = 0,
    BEGIN = 1,
    COMMIT = 2,
    ABORT = 3,
    INSERT = 4,
    DELETE = 5,
    UPDATE = 6,
};

struct LogRecord
{
    lsn_t lsn = INVALID_LSN;
    txn_id_t txn_id = INVALID_TXN_ID;
    LogType type = LogType::INVALID;
    std::string table_name;
    page_id_t page_id = INVALID_PAGE_ID;
    slot_id_t slot_id = 0;
    std::vector<char> old_data; // For UNDO (DELETE, UPDATE)
    std::vector<char> new_data; // For REDO (INSERT, UPDATE)

    // Serialization for writing to log file
    std::vector<char> serialize() const;
    // Deserialization for reading from log file
    static LogRecord deserialize(const char * data, uint32_t len);
    // Total serialized size
    uint32_t serialized_size() const;
};

class RedoLogManager
{
public:
    explicit RedoLogManager(const std::string & log_file_path);
    ~RedoLogManager();

    // Append a log record to the log buffer, returns the assigned LSN
    lsn_t append(const LogRecord & record);

    // Flush log buffer to disk (fsync)
    void flush();

    // Flush all records up to the given LSN
    void flush_to_lsn(lsn_t target_lsn);

    // Get current (next) LSN
    lsn_t current_lsn() const { return next_lsn_; }

    // Get flushed LSN (all records up to this LSN are durable)
    lsn_t flushed_lsn() const { return flushed_lsn_; }

    // Read all log records from the log file (for recovery)
    std::vector<LogRecord> read_all_records();

    // Truncate the log file (after checkpoint)
    void truncate();

private:
    std::string log_file_path_;
    std::mutex mutex_;
    lsn_t next_lsn_ = 1; // LSN 0 is invalid
    lsn_t flushed_lsn_ = 0;
    std::vector<char> log_buffer_;
    std::ofstream log_file_;

    void open_log_file();
};

} // namespace popo::storage
