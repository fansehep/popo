#include "src/trancation/manager.h"

namespace popo::txn
{

TxnManager::TxnManager(popo::storage::RedoLogManager * redo_log) : redo_log_(redo_log)
{
}

Transaction * TxnManager::begin_txn()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto & entry : active_txns_)
    {
        if (entry.second->is_running())
        {
            throw std::runtime_error("only one transaction is supported");
        }
    }

    auto txn_id = next_txn_id_.fetch_add(1);
    auto txn = std::make_unique<Transaction>(txn_id);

    // Log BEGIN record
    popo::storage::LogRecord begin_record;
    begin_record.txn_id = txn_id;
    begin_record.type = popo::storage::LogType::BEGIN;
    auto lsn = redo_log_->append(begin_record);
    txn->set_start_lsn(lsn);

    Transaction * raw_ptr = txn.get();
    active_txns_.emplace(txn_id, std::move(txn));
    return raw_ptr;
}

void TxnManager::commit(Transaction * txn)
{
    // Log COMMIT record
    popo::storage::LogRecord commit_record;
    commit_record.txn_id = txn->txn_id();
    commit_record.type = popo::storage::LogType::COMMIT;
    redo_log_->append(commit_record);

    // Flush WAL to ensure durability
    redo_log_->flush();

    txn->set_state(TxnState::COMMITTED);
    remove_txn(txn->txn_id());
}

void TxnManager::abort(Transaction * txn)
{
    // Log ABORT record
    popo::storage::LogRecord abort_record;
    abort_record.txn_id = txn->txn_id();
    abort_record.type = popo::storage::LogType::ABORT;
    redo_log_->append(abort_record);

    redo_log_->flush();

    txn->set_state(TxnState::ABORTED);
    remove_txn(txn->txn_id());
}

Transaction * TxnManager::get_txn(popo::storage::txn_id_t txn_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_txns_.find(txn_id);
    if (it != active_txns_.end())
    {
        return it->second.get();
    }
    return nullptr;
}

void TxnManager::remove_txn(popo::storage::txn_id_t txn_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    active_txns_.erase(txn_id);
}

} // namespace popo::txn
