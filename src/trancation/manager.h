#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <stdexcept>
#include "src/storage/redo_log.h"
#include "src/trancation/txn.h"

namespace popo::txn
{

class TxnManager
{
public:
    explicit TxnManager(popo::storage::RedoLogManager * redo_log);

    // Begin the single active transaction. A second begin throws.
    Transaction * begin_txn();

    // Commit a transaction: log COMMIT, flush WAL
    void commit(Transaction * txn);

    // Abort a transaction. Page rollback is done by StorageEngine; this only
    // records the abort.
    void abort(Transaction * txn);

    // Get a transaction by ID
    Transaction * get_txn(popo::storage::txn_id_t txn_id);

    // Remove completed transaction from active map
    void remove_txn(popo::storage::txn_id_t txn_id);

private:
    popo::storage::RedoLogManager * redo_log_;
    std::atomic<popo::storage::txn_id_t> next_txn_id_{1};
    std::mutex mutex_;
    std::unordered_map<popo::storage::txn_id_t, std::unique_ptr<Transaction>> active_txns_;
};

} // namespace popo::txn
