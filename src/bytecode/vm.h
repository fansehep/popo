#pragma once
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "base/error.h"
#include "bytecode/program.h"
#include "bytecode/txn_session.h"
#include "catalog/schema_mgr.h"
#include "src/storage/storage_engine.h"
#include "src/storage/table_heap.h"
#include "src/trancation/mvcc.h"
#include "src/trancation/txn.h"
#include "type/tuple.h"
#include "type/value.h"

namespace popo::bytecode
{

struct ExecuteSuccess
{
    std::vector<std::shared_ptr<popo::type::Row>> rows;
    std::vector<std::string> columns;
    std::string message;
    int64_t rows_affected = 0;
};

class VirtualMachine
{
public:
    VirtualMachine(popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine);

    // outer_txn == nullptr: this call begins and commits one transaction.
    // Otherwise the caller owns the transaction and commits or aborts it.
    popo::base::Result<ExecuteSuccess> execute(const Program & program, popo::txn::Transaction * outer_txn = nullptr);

    // Connection state backing @@transaction_isolation and @@autocommit. Without
    // it the VM reports the engine defaults.
    void set_session(const TxnSession * session) { session_ = session; }

private:
    popo::catalog::SchemaManager & schema_mgr_;
    popo::storage::StorageEngine & engine_;
    const TxnSession * session_ = nullptr;
    popo::txn::Transaction * txn_ = nullptr;
    bool own_txn_ = true;

    // Execution state
    std::vector<popo::type::Value> stack_;
    int32_t pc_ = 0;

    // Cursor management
    struct Cursor
    {
        std::string table_name;
        // Schema and secondary-index access only; scan rows stream from MVCC.
        popo::storage::TableHeap * heap = nullptr;
        int32_t pk_col = -1;
        std::unique_ptr<popo::txn::MvccManager::Cursor> stream;
        // Point/index seeks use this as their small result buffer. Streaming scans
        // keep exactly their current row here so existing opcodes share one path.
        std::vector<popo::type::Row> rows;
        size_t pos = 0;
        bool has_current = false;
    };
    std::unordered_map<int32_t, Cursor> cursors_;

    // Sorter management
    struct SortKey
    {
        int32_t column_index = 0;
        bool ascending = true;
    };
    struct Sorter
    {
        std::vector<std::shared_ptr<popo::type::Row>> rows;
        std::vector<SortKey> sort_keys;
        int32_t current_row = -1;
    };
    std::unordered_map<int32_t, Sorter> sorters_;

    // Aggregation management
    struct AggState
    {
        int32_t func_type = 0;
        popo::type::Value result;
        int64_t count = 0;
        bool initialized = false;
    };
    std::vector<AggState> agg_states_;

    // Pending row for MakeRow/InsertRow
    std::optional<popo::type::Row> pending_row_;

    // Result collection
    ExecuteSuccess result_;

    // Helper: abort txn and return unexpected error
    auto error(const popo::base::Error & err)
    {
        if (txn_)
        {
            if (own_txn_)
            {
                engine_.abort_txn(txn_);
            }
            txn_ = nullptr;
        }
        return std::unexpected(err);
    }

    // Stack operations
    void push(const popo::type::Value & val);
    popo::type::Value pop();
    const popo::type::Value & peek() const;

    // Reset state for new execution
    void reset();

    // Instruction loop. Errors from buffered writes are propagated as Result.
    popo::base::Result<ExecuteSuccess> run(const Program & program);
};

} // namespace popo::bytecode
