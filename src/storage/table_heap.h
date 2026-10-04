#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "src/catalog/schema.h"
#include "src/storage/btree_page.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/common.h"
#include "src/storage/page.h"
#include "src/storage/redo_log.h"
#include "src/storage/tuple.h"
#include "src/trancation/txn.h"
#include "src/type/tuple.h"
#include "src/type/value.h"

namespace neon::storage
{

// Forward declaration
class TableHeap;

// Iterates all valid (non-deleted) tuples across all leaf pages of the B+Tree
class TableIterator
{
public:
    TableIterator(TableHeap * heap, const neon::catalog::Schema & schema);

    // Reset to first tuple of leftmost leaf page
    void rewind();

    // Advance to next valid tuple
    bool next();

    // Is iterator pointing to a valid tuple?
    bool valid() const;

    // Physical tuple at the current slot. Deform with schema() when a Value is needed.
    Tuple current_tuple() const;

    // Deformed view of current_tuple(), for the executor.
    neon::type::Row current_row() const;

    // Get current RowId
    RowId current_row_id() const;

private:
    TableHeap * heap_;
    neon::catalog::Schema schema_;
    page_id_t current_page_id_ = INVALID_PAGE_ID;
    slot_id_t current_slot_id_ = 0;
    bool valid_ = false;
    PageGuard page_guard_;

    // Skip to next valid slot (within current page or move to next leaf page)
    void advance_to_valid();
};

// Clustered index storage: B+Tree organized by primary key, row data in leaf pages.
// External API unchanged from heap-based version.
class TableHeap
{
    friend class TableIterator;

public:
    TableHeap(
        const std::string & table_name, BufferPoolManager * buffer_pool, RedoLogManager * redo_log, const neon::catalog::Schema & schema);

    // Insert a physical tuple into the B+Tree, returns RowId. Writes redo log.
    RowId insert_row(neon::txn::Transaction * txn, const Tuple & tuple);

    // Delete a row by RowId. Writes redo log.
    bool delete_row(neon::txn::Transaction * txn, RowId rid);

    // Update a row by RowId. Writes redo log.
    RowId update_row(neon::txn::Transaction * txn, RowId rid, const Tuple & tuple);

    // Get the physical tuple at RowId.
    std::optional<Tuple> get_row(RowId rid) const;

    // Count live tuples by walking leaf pages once.
    int64_t count_rows() const;

    // Look up a row by primary key value using B+Tree search.
    // Returns RowId of the matching row, or nullopt if not found.
    std::optional<RowId> lookup_pk(const neon::type::Value & pk) const;

    // Create an iterator for full table scan (leaf page linked list, PK order)
    TableIterator iter() const;

    // Get table name
    const std::string & table_name() const { return table_name_; }

    // Get schema
    const neon::catalog::Schema & schema() const { return schema_; }

    // Get buffer pool
    BufferPoolManager * buffer_pool() const { return buffer_pool_; }

    // Get B+Tree root page id (for testing/debugging)
    page_id_t get_root_page_id() const { return root_page_id_; }
    void set_root_page_id(page_id_t pid) { root_page_id_ = pid; }

private:
    std::string table_name_;
    BufferPoolManager * buffer_pool_;
    RedoLogManager * redo_log_;
    neon::catalog::Schema schema_;
    mutable std::mutex mutex_;

    page_id_t root_page_id_ = INVALID_PAGE_ID; // B+Tree root page

    // --- PK helpers ---
    neon::type::Value extract_pk(const Tuple & tuple) const;
    neon::type::Value extract_pk_from_serialized(const char * data, uint16_t len) const;
    int compare_pk(const neon::type::Value & a, const neon::type::Value & b) const;
    // Compare the primary-key column of two physical tuples. Fixed-width keys
    // at column 0 are read from the bytes; Schema supplies the type.
    int compare_tuple_pk(const char * a, uint16_t a_len, const char * b, uint16_t b_len) const;

    // --- B+Tree navigation ---
    // Search from root to find the leaf page that should contain the given key,
    // also returns the path of internal page_ids from root to leaf's parent.
    // path[0] = root, path[last] = leaf's parent (internal node), or empty if root is leaf.
    page_id_t search_leaf_with_path(const neon::type::Value & pk, std::vector<page_id_t> & path) const;

    // --- B+Tree insert ---
    // Insert serialized row into the leaf page. Handles split and upward propagation.
    // Returns {slot_id, leaf page_id} of the inserted tuple.
    std::pair<slot_id_t, page_id_t> btree_insert(const char * data, uint16_t len, const neon::type::Value & pk);

    // Insert key+child into internal node, handling splits recursively.
    // path: stack of internal page_ids from root down to the target node's parent.
    // path_idx: index into path where the target page sits.
    void insert_internal_with_path(
        const char * key_data, uint16_t key_len, page_id_t child_page, std::vector<page_id_t> & path, size_t path_idx);

    // --- Leaf page management ---
    page_id_t create_new_leaf(page_id_t prev_leaf);
    page_id_t find_leftmost_leaf() const;
};

} // namespace neon::storage
