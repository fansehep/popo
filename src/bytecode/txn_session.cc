#include "bytecode/txn_session.h"

#include "base/error.h"
#include "src/storage/storage_engine.h"

namespace popo::bytecode
{

popo::txn::IsolationLevel to_storage_isolation(popo::parser::IsolationLevel level)
{
    switch (level)
    {
        case popo::parser::IsolationLevel::ReadUncommitted:
            return popo::txn::IsolationLevel::ReadUncommitted;
        case popo::parser::IsolationLevel::ReadCommitted:
            return popo::txn::IsolationLevel::ReadCommitted;
        case popo::parser::IsolationLevel::RepeatableRead:
            return popo::txn::IsolationLevel::RepeatableRead;
        case popo::parser::IsolationLevel::Serializable:
            return popo::txn::IsolationLevel::Serializable;
    }
    return popo::txn::IsolationLevel::ReadCommitted;
}

popo::parser::IsolationLevel to_sql_isolation(popo::txn::IsolationLevel level)
{
    switch (level)
    {
        case popo::txn::IsolationLevel::ReadUncommitted:
            return popo::parser::IsolationLevel::ReadUncommitted;
        case popo::txn::IsolationLevel::ReadCommitted:
            return popo::parser::IsolationLevel::ReadCommitted;
        case popo::txn::IsolationLevel::RepeatableRead:
            return popo::parser::IsolationLevel::RepeatableRead;
        case popo::txn::IsolationLevel::Serializable:
            return popo::parser::IsolationLevel::Serializable;
    }
    return popo::parser::IsolationLevel::ReadCommitted;
}

TxnSession::TxnSession(popo::storage::StorageEngine & engine)
    : engine_(engine)
{
}

TxnSession::~TxnSession()
{
    rollback();
}

popo::parser::IsolationLevel TxnSession::isolation() const
{
    if (txn_ != nullptr)
    {
        return to_sql_isolation(txn_->isolation());
    }
    return next_isolation_.value_or(session_isolation());
}

popo::parser::IsolationLevel TxnSession::session_isolation() const
{
    return isolation_.value_or(to_sql_isolation(engine_.default_isolation()));
}

popo::base::Result<void> TxnSession::set_session_isolation(popo::parser::IsolationLevel level)
{
    if (txn_ != nullptr)
    {
        return std::unexpected(popo::base::make_error(
            popo::base::Errno::InvalidArg,
            "Transaction characteristics can't be changed while a transaction is in progress"));
    }
    isolation_ = level;
    next_isolation_.reset();
    return {};
}

popo::base::Result<void> TxnSession::set_autocommit(bool on)
{
    if (on == autocommit_)
    {
        return {};
    }
    autocommit_ = on;
    if (on)
    {
        return commit();
    }
    if (txn_ == nullptr)
    {
        txn_ = engine_.begin_txn(to_storage_isolation(take_level()));
    }
    return {};
}

popo::parser::IsolationLevel TxnSession::take_level()
{
    if (next_isolation_)
    {
        auto level = *next_isolation_;
        next_isolation_.reset();
        return level;
    }
    return session_isolation();
}

popo::base::Result<void> TxnSession::begin(std::optional<popo::parser::IsolationLevel> level)
{
    auto chosen = take_level();
    if (level)
    {
        chosen = *level;
    }
    if (txn_ != nullptr)
    {
        auto * open = txn_;
        txn_ = nullptr;
        if (open->is_running())
        {
            auto committed = engine_.commit_txn(open);
            if (!committed)
            {
                return std::unexpected(committed.error());
            }
        }
    }
    explicit_ = true;
    txn_ = engine_.begin_txn(to_storage_isolation(chosen));
    return {};
}

popo::base::Result<void> TxnSession::commit()
{
    explicit_ = false;
    if (txn_ == nullptr)
    {
        return {};
    }
    auto * open = txn_;
    // The engine destroys the Transaction on commit/abort, so drop our pointer
    // first: a serialization failure aborts and frees it before throwing.
    txn_ = nullptr;
    if (open->is_running())
    {
        return engine_.commit_txn(open);
    }
    return {};
}

void TxnSession::rollback()
{
    explicit_ = false;
    if (txn_ == nullptr)
    {
        return;
    }
    auto * open = txn_;
    txn_ = nullptr;
    if (open->is_running())
    {
        engine_.abort_txn(open);
    }
}

popo::base::Result<popo::txn::Transaction *> TxnSession::statement_txn()
{
    if (txn_ == nullptr)
    {
        explicit_ = false;
        txn_ = engine_.begin_txn(to_storage_isolation(take_level()));
    }
    return txn_;
}

popo::base::Result<void> TxnSession::statement_succeeded()
{
    if (autocommit_ && !explicit_)
    {
        return commit();
    }
    return {};
}

void TxnSession::statement_failed()
{
    rollback();
}

} // namespace popo::bytecode