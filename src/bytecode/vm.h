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

namespace neon::bytecode
{

struct ExecuteSuccess
{
    std::vector<std::shared_ptr<neon::type::Row>> rows;
    std::vector<std::string> columns;
    std::string message;
    int64_t rows_affected = 0;
};

class VirtualMachine
{
public:
    VirtualMachine(neon::catalog::SchemaManager & schema_mgr, neon::storage::StorageEngine & engine);

    // outer_txn == nullptr: this call begins and commits one transaction.
    // Otherwise the caller owns the transaction and commits or aborts it.
    neon::base::Result<ExecuteSuccess> execute(const Program & program, neon::txn::Transaction * outer_txn = nullptr);

private:
    neon::catalog::SchemaManager & schema_mgr_;
    neon::storage::StorageEngine & engine_;
    neon::txn::Transaction * txn_ = nullptr;
    bool own_txn_ = true;

    // Execution state
    std::vector<neon::type::Value> stack_;
    int32_t pc_ = 0;

    // Cursor management
    struct Cursor
    {
        std::string table_name;
        neon::storage::TableHeap * heap = nullptr;
        std::unique_ptr<neon::storage::TableIterator> iter;
        neon::catalog::Schema schema;
        bool is_write = false;
        // For tracking current row during iteration
        neon::storage::RowId current_rid;
        neon::type::Row current_row_cache;
        bool has_current = false;
        bool row_materialized = false;
        bool index_scan = false;
        size_t index_pos = 0;
        std::vector<neon::storage::RowId> index_hits;
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
        std::vector<std::shared_ptr<neon::type::Row>> rows;
        std::vector<SortKey> sort_keys;
        int32_t current_row = -1;
    };
    std::unordered_map<int32_t, Sorter> sorters_;

    // Aggregation management
    struct AggState
    {
        int32_t func_type = 0;
        neon::type::Value result;
        int64_t count = 0;
        bool initialized = false;
    };
    std::vector<AggState> agg_states_;

    // Pending row for MakeRow/InsertRow
    std::optional<neon::type::Row> pending_row_;

    // Result collection
    ExecuteSuccess result_;

    // Helper: abort txn and return unexpected error
    auto error(const neon::base::Error & err)
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
    void push(const neon::type::Value & val);
    neon::type::Value pop();
    const neon::type::Value & peek() const;

    // Reset state for new execution
    void reset();
};

} // namespace neon::bytecode
