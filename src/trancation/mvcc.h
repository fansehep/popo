#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "src/base/error.h"
#include "src/storage/common.h"
#include "src/storage/redo_log.h"
#include "src/storage/table_heap.h"
#include "src/trancation/txn.h"
#include "src/type/tuple.h"

namespace popo::storage
{
class StorageEngine;
}

namespace popo::txn
{

// Multi-version concurrency control with ARIES write-ahead logging.
//
// The B+Tree heap is the read authority for the current committed image.
// versions_ contains only touched-key history while an active snapshot needs it.
// Writers buffer their changes in a per-transaction overlay and only
// publish them at commit (no-steal), so a crash leaves no uncommitted data on
// shared pages. Recovery repeats history (redo of committed work) and rolls
// back losers.
class MvccManager
{
public:
    class Cursor
    {
    public:
        Cursor(Cursor &&) noexcept;
        Cursor & operator=(Cursor &&) noexcept;
        ~Cursor();

        // Produces the next visible row in primary-key order.
        bool next(popo::type::Row & row);

    private:
        struct Impl;
        explicit Cursor(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
        friend class MvccManager;
    };

    MvccManager(popo::storage::StorageEngine * engine, popo::storage::RedoLogManager * redo_log);
    ~MvccManager();

    // Transaction lifecycle. begin captures a snapshot per isolation level.
    Transaction * begin(IsolationLevel level);
    popo::base::Result<void> commit(Transaction * txn);
    void abort(Transaction * txn);

    // Refresh the statement snapshot. Read-committed reads one stable image for
    // the whole statement; repeatable-read and serializable keep the txn snapshot.
    void begin_statement(Transaction * txn);

    Transaction * get_txn(popo::storage::txn_id_t id);
    IsolationLevel default_isolation() const { return default_isolation_; }
    void set_default_isolation(IsolationLevel level) { default_isolation_ = level; }

    // Buffered DML (logged to WAL, applied to the overlay; installed at commit).
    // `pk` is the primary-key value identifying the row.
    popo::base::Result<void> stage_insert(Transaction * txn, const std::string & table, const popo::type::Row & row);
    popo::base::Result<void>
    stage_update(Transaction * txn, const std::string & table, const popo::type::Value & pk, const popo::type::Row & row);
    popo::base::Result<void> stage_delete(Transaction * txn, const std::string & table, const popo::type::Value & pk);

    // Snapshot reads. The cursor retains only its current row and merges the
    // B+Tree, incremental history, and own overlay in primary-key order.
    Cursor scan_cursor(
        Transaction * txn, const std::string & table, const std::optional<popo::type::Value> & low = std::nullopt,
        const std::optional<popo::type::Value> & high = std::nullopt);
    std::vector<popo::type::Row> scan(Transaction * txn, const std::string & table);
    int64_t count(Transaction * txn, const std::string & table);
    std::optional<popo::type::Row> lookup(Transaction * txn, const std::string & table, const popo::type::Value & pk);
    // Latest committed row, without opening a transaction. Equivalent to a
    // single-statement autocommit read at any isolation level, and lets the
    // point-select fast path skip BEGIN/COMMIT WAL traffic.
    std::optional<popo::type::Row> lookup_committed(const std::string & table, const popo::type::Value & pk);

    // Record a read range for serializable validation.
    void note_read(Transaction * txn, const std::string & table, const std::optional<popo::type::Value> & low,
                   const std::optional<popo::type::Value> & high);

    // Current committed CSN (read-committed statement snapshot).
    uint64_t current_csn() const { return next_csn_.load(std::memory_order_acquire); }

    // CSN this transaction reads at right now: its statement snapshot for
    // read-committed, its transaction snapshot for RR/serializable, UINT64_MAX
    // for dirty reads. Equal to current_csn() only when the read image is the
    // current committed heap, which is what index-assisted access requires.
    uint64_t snapshot_csn_for(Transaction * txn);

    // ---- Persistence / recovery hooks ----
    // Recovery leaves the committed baseline in the B+Tree; no table copy is made.
    void rebuild_from_heap(const std::string & table);
    void forget_table(const std::string & table);
    // Latest committed row per primary key (for checkpoint compaction).
    std::vector<popo::type::Row> current_rows(const std::string & table);
    // Install a committed overlay into the heap without re-logging (recovery redo
    // and normal commit both use this). Returns false on storage failure.
    bool install_row(const std::string & table, const popo::type::Row & row);
    bool remove_row(const std::string & table, const popo::type::Value & pk);

    // Drop version history no active snapshot can observe.
    void garbage_collect();

    // Test/diagnostic hook: number of rows retained in incremental history.
    size_t retained_version_count() const;

private:
    struct Version
    {
        std::optional<popo::type::Row> row; // disengaged is a committed tombstone
        uint64_t begin_csn = 0; // commit that created this version
        uint64_t end_csn = UINT64_MAX; // commit that deleted/superseded it
    };
    struct ValueLess
    {
        bool operator()(const popo::type::Value & a, const popo::type::Value & b) const;
    };
    using VersionChain = std::vector<Version>;
    using VersionedTable = std::map<popo::type::Value, VersionChain, ValueLess>;

    struct TxnStatus
    {
        TxnState state = TxnState::RUNNING;
        uint64_t csn = 0;
    };

    popo::storage::StorageEngine * engine_;
    popo::storage::RedoLogManager * redo_log_;

    mutable std::shared_mutex version_mutex_;
    std::unordered_map<std::string, VersionedTable> versions_;

    std::mutex txn_mutex_;
    std::atomic<popo::storage::txn_id_t> next_txn_id_{1};
    std::atomic<uint64_t> next_csn_{1};
    std::unordered_map<popo::storage::txn_id_t, std::unique_ptr<Transaction>> active_;
    // Retained status of finished transactions so in-flight snapshots can still
    // resolve visibility. Pruned by garbage_collect().
    std::unordered_map<popo::storage::txn_id_t, TxnStatus> status_;
    IsolationLevel default_isolation_ = IsolationLevel::ReadCommitted;

    // Serializes commit install so the heap and version store publish atomically.
    std::mutex commit_mutex_;

    popo::base::Result<popo::type::Value> primary_key_of(const std::string & table, const popo::type::Row & row) const;
    bool visible(const Version & v, uint64_t snapshot_csn) const;
    std::optional<popo::type::Row> visible_row(Transaction * txn, const std::string & table, const popo::type::Value & pk,
                                               uint64_t snapshot_csn);
    void log_operation(Transaction * txn, popo::storage::LogType type, const std::string & table,
                       const std::vector<char> & old_data, const std::vector<char> & new_data);
    void publish_overlay(Transaction * txn, const std::string & table, const popo::type::Value & pk,
                         const std::optional<popo::type::Row> & row);
    // Unique secondary keys are enforced when the write is buffered, not when it
    // is installed: by then the heap has already accepted the row.
    popo::base::Result<void> check_unique(Transaction * txn, const std::string & table, const popo::type::Row & row);
    // Whether a buffered insert would collide on the primary key.
    bool primary_key_taken(Transaction * txn, const std::string & table, const popo::type::Value & pk);
    bool validate_serializable(Transaction * txn);
    // First-committer-wins on the write set: a key this txn intends to write must
    // not have been changed by a commit after its snapshot, else the write would
    // be based on a stale version (lost update).
    bool has_write_conflict(Transaction * txn);
    void install_overlay(Transaction * txn, uint64_t csn);
    // abort() with commit_mutex_ already held (commit-time validation failure).
    void abort_locked(Transaction * txn);
};

} // namespace popo::txn