#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "src/base/error.h"
#include "src/storage/redo_log.h"
#include "src/trancation/txn.h"

namespace popo::txn
{

class TxnManager
{
public:
    explicit TxnManager(popo::storage::RedoLogManager * redo_log);

    popo::base::Result<Transaction *> begin_txn();

    void commit(Transaction * txn);

    void abort(Transaction * txn);

    Transaction * get_txn(popo::storage::txn_id_t txn_id);

    void remove_txn(popo::storage::txn_id_t txn_id);

private:
    popo::storage::RedoLogManager * redo_log_;
    std::atomic<popo::storage::txn_id_t> next_txn_id_{1};
    std::mutex mutex_;
    std::unordered_map<popo::storage::txn_id_t, std::unique_ptr<Transaction>> active_txns_;
};

} // namespace popo::txn
