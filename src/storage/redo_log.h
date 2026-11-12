#pragma once
#include <condition_variable>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "src/storage/common.h"

namespace popo::storage

{

// ARIES-style log record types. UPDATE is logged as a single record carrying
// both the before and after image; CLR is a compensation record written during
// undo so that recovery can repeat history without re-undoing finished work.
enum class LogType : uint8_t
{
    INVALID = 0,
    BEGIN = 1,
    COMMIT = 2,
    ABORT = 3,
    INSERT = 4,
    DELETE = 5,
    UPDATE = 6,
    END = 7,
    CLR = 8,
    CHECKPOINT = 9,
    CREATE_TABLE = 10,
    DROP_TABLE = 11,
    CREATE_INDEX = 12,
};

struct LogRecord
{
    lsn_t lsn = INVALID_LSN;
    // Previous LSN produced by the same transaction; forms the undo chain.
    lsn_t prev_lsn = INVALID_LSN;
    txn_id_t txn_id = INVALID_TXN_ID;
    LogType type = LogType::INVALID;
    std::string table_name;
    page_id_t page_id = INVALID_PAGE_ID;
    slot_id_t slot_id = 0;
    std::vector<char> old_data; // Before image (DELETE, UPDATE, undo of INSERT)
    std::vector<char> new_data; // After image (INSERT, UPDATE, undo of DELETE)

    // CLR only: the next LSN to undo after this compensation, and the effective
    // operation the compensation performs (so redo can replay it).
    lsn_t undo_next_lsn = INVALID_LSN;
    LogType compensation = LogType::INVALID;

    // CHECKPOINT only: encoded active transaction table (see RecoveryManager).
    // Stored in new_data; kept here as a structured view for convenience.
    struct ActiveTxn
    {
        txn_id_t txn_id = INVALID_TXN_ID;
        lsn_t last_lsn = INVALID_LSN;
        uint8_t status = 0; // 0 running, 1 commit-in-progress
    };
    std::vector<ActiveTxn> checkpoint_txns;

    std::vector<char> serialize() const;
    static LogRecord deserialize(const char * data, uint32_t len);
    uint32_t serialized_size() const;
};

// Write-ahead log with ARIES semantics: monotonically increasing LSNs, a WAL
// rule enforced through flush_to_lsn(), group commit, and fuzzy checkpoints.
class RedoLogManager
{
public:
    explicit RedoLogManager(const std::string & log_file_path);
    ~RedoLogManager();

    RedoLogManager(const RedoLogManager &) = delete;
    RedoLogManager & operator=(const RedoLogManager &) = delete;

    // Append a record to the in-memory log buffer; returns its assigned LSN.
    lsn_t append(const LogRecord & record);

    // Flush the whole buffer to disk and fsync.
    void flush();

    // WAL rule: block until every record up to target_lsn is durable.
    void flush_to_lsn(lsn_t target_lsn);

    lsn_t current_lsn() const { return next_lsn_; }
    lsn_t flushed_lsn() const { return flushed_lsn_; }

    // Read every durable record (for the recovery analysis pass).
    std::vector<LogRecord> read_all_records();

    // Drop the log file and reset LSN numbering (after a checkpoint publishes
    // all dirty pages, the log prefix is no longer needed).
    void truncate();

private:
    std::string log_file_path_;
    std::mutex mutex_;
    std::condition_variable flush_cv_;
    lsn_t next_lsn_ = 1; // LSN 0 is invalid
    lsn_t flushed_lsn_ = 0;
    std::vector<char> log_buffer_;
    std::ofstream log_file_;
    int log_fd_ = -1;

    void open_log_file();
    void flush_locked();
    void persist_watermark();
};

} // namespace popo::storage