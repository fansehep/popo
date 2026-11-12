#pragma once

#include <optional>
#include "base/error.h"
#include "parser/ast.h"
#include "src/storage/storage_engine.h"
#include "src/trancation/txn.h"

namespace popo::bytecode
{

// The SQL-facing and storage-facing isolation enums are deliberately separate so
// the parser does not depend on storage; these are the only bridge.
popo::txn::IsolationLevel to_storage_isolation(popo::parser::IsolationLevel level);
popo::parser::IsolationLevel to_sql_isolation(popo::txn::IsolationLevel level);

// Per-connection transaction state: isolation level, autocommit, and the
// transaction carried across statements. The server Session and the SQL test
// runner both drive this so BEGIN/COMMIT/ROLLBACK behave identically in each.
class TxnSession
{
public:
    explicit TxnSession(popo::storage::StorageEngine & engine);
    ~TxnSession();

    TxnSession(const TxnSession &) = delete;
    TxnSession & operator=(const TxnSession &) = delete;

    // What @@transaction_isolation reports: the open transaction's level, else
    // the level the next transaction will get.
    popo::parser::IsolationLevel isolation() const;
    // MySQL rejects changing session characteristics mid-transaction.
    popo::base::Result<void> set_session_isolation(popo::parser::IsolationLevel level);
    void set_next_isolation(popo::parser::IsolationLevel level) { next_isolation_ = level; }

    bool autocommit() const { return autocommit_; }
    popo::base::Result<void> set_autocommit(bool on);

    bool in_transaction() const { return txn_ != nullptr; }
    bool explicit_transaction() const { return explicit_; }
    popo::txn::Transaction * current() { return txn_; }

    // BEGIN / START TRANSACTION. Implicitly commits whatever was open.
    popo::base::Result<void> begin(std::optional<popo::parser::IsolationLevel> level = std::nullopt);
    popo::base::Result<void> commit();
    void rollback();

    // Transaction for the next statement, opening an implicit one if needed.
    popo::base::Result<popo::txn::Transaction *> statement_txn();
    popo::base::Result<void> statement_succeeded();
    // A failed statement poisons the transaction: partial buffered writes must
    // not survive into the next statement.
    void statement_failed();

private:
    popo::storage::StorageEngine & engine_;
    std::optional<popo::parser::IsolationLevel> isolation_;
    std::optional<popo::parser::IsolationLevel> next_isolation_;
    bool autocommit_ = true;
    bool explicit_ = false;
    popo::txn::Transaction * txn_ = nullptr;

    // Resolved on demand: a TxnSession can be built before StorageEngine::init()
    // creates the MVCC manager that owns the global default.
    popo::parser::IsolationLevel session_isolation() const;
    popo::parser::IsolationLevel take_level();
};

} // namespace popo::bytecode