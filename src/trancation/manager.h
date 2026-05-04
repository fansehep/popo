#pragma once
#include "src/trancation/txn.h"
#include "src/storage/redo_log.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace neon::txn {

class TxnManager {
public:
    explicit TxnManager(neon::storage::RedoLogManager* redo_log);

    // Begin a new transaction
    Transaction* begin_txn();

    // Commit a transaction: log COMMIT, flush WAL
    void commit(Transaction* txn);

    // Abort a transaction: undo write_set in reverse, log ABORT
    // The undo_callback is called for each record that needs to be undone
    using UndoCallback = std::function<void(const neon::storage::LogRecord&)>;
    void abort(Transaction* txn, UndoCallback undo_fn = nullptr);

    // Get a transaction by ID
    Transaction* get_txn(neon::storage::txn_id_t txn_id);

    // Remove completed transaction from active map
    void remove_txn(neon::storage::txn_id_t txn_id);

private:
    neon::storage::RedoLogManager* redo_log_;
    std::atomic<neon::storage::txn_id_t> next_txn_id_{1};
    std::mutex mutex_;
    std::unordered_map<neon::storage::txn_id_t, std::unique_ptr<Transaction>> active_txns_;
};

}  // namespace neon::txn
