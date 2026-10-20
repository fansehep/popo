#pragma once
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "base/error.h"
#include "bytecode/program.h"
#include "catalog/schema_mgr.h"
#include "src/storage/storage_engine.h"
#include "src/storage/table_heap.h"
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

private:
    popo::catalog::SchemaManager & schema_mgr_;
    popo::storage::StorageEngine & engine_;
    popo::txn::Transaction * txn_ = nullptr;
    bool own_txn_ = true;

    // Execution state
    std::vector<popo::type::Value> stack_;
    int32_t pc_ = 0;

    // Cursor management
    struct Cursor
    {
        std::string table_name;
        popo::storage::TableHeap * heap = nullptr;
        std::unique_ptr<popo::storage::TableIterator> iter;
        popo::catalog::Schema schema;
        bool is_write = false;
        // For tracking current row during iteration
        popo::storage::RowId current_rid;
        popo::type::Row current_row_cache;
        bool has_current = false;
        bool row_materialized = false;
        bool index_scan = false;
        size_t index_pos = 0;
        std::vector<popo::storage::RowId> index_hits;
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
};

} // namespace popo::bytecode
