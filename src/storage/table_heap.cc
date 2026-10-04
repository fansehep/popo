#include "src/storage/table_heap.h"
#include <cstring>
#include <spdlog/spdlog.h>

namespace neon::storage
{

namespace
{

bool is_null_value(const neon::type::Value & v)
{
    return v.datatype() == neon::type::ValueType::Null;
}

int64_t read_i64(const neon::type::Value & v)
{
    using neon::type::ValueType;
    switch (v.datatype())
    {
        case ValueType::Uint8:
            return v.u8();
        case ValueType::Uint16:
            return v.u16();
        case ValueType::Uint32:
            return v.u32();
        case ValueType::Uint64:
            return static_cast<int64_t>(v.u64());
        case ValueType::Int8:
            return v.i8();
        case ValueType::Int16:
            return v.i16();
        case ValueType::Int32:
            return v.i32();
        case ValueType::Int64:
            return v.i64();
        case ValueType::Bool:
            return v.boolean() ? 1 : 0;
        case ValueType::Char:
            return v.u8();
        case ValueType::Float32:
            return static_cast<int64_t>(v.f32());
        case ValueType::Float64:
            return static_cast<int64_t>(v.f64());
        default:
            return 0;
    }
}

double read_f64(const neon::type::Value & v)
{
    using neon::type::ValueType;
    switch (v.datatype())
    {
        case ValueType::Float32:
            return v.f32();
        case ValueType::Float64:
            return v.f64();
        default:
            return static_cast<double>(read_i64(v));
    }
}

int cmp_i64(int64_t a, int64_t b)
{
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

// Column type comes from Schema. Each Value is only a payload at this boundary,
// so the read switch follows the value's own tag (an Int64 literal and an Int32
// column still compare as integers).
int compare_as(neon::type::ValueType column_type, const neon::type::Value & a, const neon::type::Value & b)
{
    using neon::type::ValueType;
    if (is_null_value(a) && is_null_value(b))
        return 0;
    if (is_null_value(a))
        return -1;
    if (is_null_value(b))
        return 1;

    switch (column_type)
    {
        case ValueType::String:
            if (a.string_ref() < b.string_ref())
                return -1;
            if (a.string_ref() > b.string_ref())
                return 1;
            return 0;
        case ValueType::Float32:
        case ValueType::Float64: {
            double x = read_f64(a);
            double y = read_f64(b);
            if (x < y)
                return -1;
            if (x > y)
                return 1;
            return 0;
        }
        case ValueType::Uint64: {
            auto x = static_cast<uint64_t>(read_i64(a));
            auto y = static_cast<uint64_t>(read_i64(b));
            if (x < y)
                return -1;
            if (x > y)
                return 1;
            return 0;
        }
        default:
            return cmp_i64(read_i64(a), read_i64(b));
    }
}

} // namespace


// ============================================================================
// TableHeap — Clustered B+Tree Index
// ============================================================================

TableHeap::TableHeap(
    const std::string & table_name, BufferPoolManager * buffer_pool, RedoLogManager * redo_log, const neon::catalog::Schema & schema)
    : table_name_(table_name), buffer_pool_(buffer_pool), redo_log_(redo_log), schema_(schema)
{
}

// --- Primary Key Helpers ---

neon::type::Value TableHeap::extract_pk(const Tuple & tuple) const
{
    return extract_pk_from_serialized(tuple.data(), tuple.size());
}

neon::type::Value TableHeap::extract_pk_from_serialized(const char * data, uint16_t len) const
{
    int32_t pk_idx = schema_.primary_key_col_idx_;
    if (pk_idx < 0)
    {
        return neon::type::Value();
    }

    // Fast path: extract a fixed-width PK column directly from serialized bytes
    // without deserializing the entire row.
    if (pk_idx == 0 && len > sizeof(uint16_t))
    {
        uint16_t num_cols = 0;
        std::memcpy(&num_cols, data, sizeof(uint16_t));
        uint32_t null_bitmap_bytes = (num_cols + 7) / 8;

        if (len > sizeof(uint16_t) + null_bitmap_bytes)
        {
            // Check null bitmap: bit 0 of byte 0
            bool is_null = (data[sizeof(uint16_t)] >> 0) & 1;
            if (!is_null)
            {
                neon::type::ValueType pk_type = schema_.columns_[0].data_type_;
                const char * val_data = data + sizeof(uint16_t) + null_bitmap_bytes;

                switch (pk_type)
                {
                    case neon::type::ValueType::Int32: {
                        int32_t v = 0;
                        std::memcpy(&v, val_data, 4);
                        return neon::type::Value(neon::type::ValueType::Int32, v);
                    }
                    case neon::type::ValueType::Uint32: {
                        uint32_t v = 0;
                        std::memcpy(&v, val_data, 4);
                        return neon::type::Value(neon::type::ValueType::Uint32, v);
                    }
                    case neon::type::ValueType::Int64: {
                        int64_t v = 0;
                        std::memcpy(&v, val_data, 8);
                        return neon::type::Value(neon::type::ValueType::Int64, v);
                    }
                    case neon::type::ValueType::Uint64: {
                        uint64_t v = 0;
                        std::memcpy(&v, val_data, 8);
                        return neon::type::Value(neon::type::ValueType::Uint64, v);
                    }
                    case neon::type::ValueType::Float64: {
                        double v = 0;
                        std::memcpy(&v, val_data, 8);
                        return neon::type::Value(neon::type::ValueType::Float64, v);
                    }
                    case neon::type::ValueType::Int8: {
                        int8_t v = 0;
                        std::memcpy(&v, val_data, 1);
                        return neon::type::Value(neon::type::ValueType::Int8, v);
                    }
                    case neon::type::ValueType::Int16: {
                        int16_t v = 0;
                        std::memcpy(&v, val_data, 2);
                        return neon::type::Value(neon::type::ValueType::Int16, v);
                    }
                    default:
                        break; // Fall through to full deserialize
                }
            }
        }
    }

    // Slow path: full row deserialization
    auto row = RowSerializer::deserialize(data, len, schema_);
    if (pk_idx >= static_cast<int32_t>(row.values_.size()))
    {
        return neon::type::Value();
    }
    return std::move(row.values_[pk_idx]);
}

int TableHeap::compare_pk(const neon::type::Value & a, const neon::type::Value & b) const
{
    int32_t pk_idx = schema_.primary_key_col_idx_;
    if (pk_idx < 0 || pk_idx >= static_cast<int32_t>(schema_.columns_.size()))
    {
        return 0;
    }
    return compare_as(schema_.columns_[pk_idx].data_type_, a, b);
}

// --- B+Tree Navigation ---

page_id_t TableHeap::search_leaf_with_path(const neon::type::Value & pk, std::vector<page_id_t> & path) const
{
    path.clear();
    if (root_page_id_ == INVALID_PAGE_ID)
    {
        return INVALID_PAGE_ID;
    }

    page_id_t current = root_page_id_;

    while (true)
    {
        auto guard = buffer_pool_->fetch_page(table_name_, current);
        if (!guard.is_valid())
        {
            return INVALID_PAGE_ID;
        }

        const auto * hdr = reinterpret_cast<const PageHeader *>(guard.data());
        auto page_type = static_cast<PageType>(hdr->page_type);

        if (page_type == PageType::BPTREE_LEAF_PAGE || page_type == PageType::DATA_PAGE)
        {
            return current; // path contains all internal nodes above this leaf
        }

        if (page_type == PageType::BPTREE_INTERNAL_PAGE)
        {
            path.push_back(current); // Record this internal node in the path

            const auto * internal = reinterpret_cast<const BTreeInternalPage *>(guard.data());

            auto cmp = [this](const char * a_data, uint16_t a_len, const char * b_data, uint16_t b_len) -> int
            {
                auto key_a = extract_pk_from_serialized(a_data, a_len);
                auto key_b = extract_pk_from_serialized(b_data, b_len);
                return compare_pk(key_a, key_b);
            };

            neon::type::Row key_row;
            int32_t ncols = static_cast<int32_t>(schema_.columns_.size());
            int32_t pk_idx = schema_.primary_key_col_idx_;
            key_row.reserve_values(static_cast<size_t>(ncols));
            for (int32_t i = 0; i < ncols; ++i)
            {
                if (i == pk_idx)
                    key_row.append_value(pk);
                else
                    key_row.append_value(neon::type::Value());
            }
            auto key = Tuple::form(schema_, key_row);

            current = internal->find_child(key.data(), key.size(), cmp);
        }
        else
        {
            return INVALID_PAGE_ID;
        }
    }
}

// --- Leaf Page Management ---

page_id_t TableHeap::create_new_leaf(page_id_t prev_leaf)
{
    page_id_t new_pid = 0;
    auto guard = buffer_pool_->new_page(table_name_, new_pid);
    if (!guard.is_valid())
    {
        return INVALID_PAGE_ID;
    }
    auto * page = reinterpret_cast<DataPage *>(guard.data());
    page->init_as_leaf(new_pid);

    // Link previous leaf to this new one
    if (prev_leaf != INVALID_PAGE_ID)
    {
        auto prev_guard = buffer_pool_->fetch_page(table_name_, prev_leaf);
        if (prev_guard.is_valid())
        {
            auto * prev_page = reinterpret_cast<DataPage *>(prev_guard.data());
            page->set_next_leaf_page(prev_page->next_leaf_page());
            prev_page->set_next_leaf_page(new_pid);
            prev_guard.mark_dirty();
        }
    }

    guard.mark_dirty();
    return new_pid;
}

page_id_t TableHeap::find_leftmost_leaf() const
{
    if (root_page_id_ == INVALID_PAGE_ID)
        return INVALID_PAGE_ID;

    page_id_t current = root_page_id_;
    while (true)
    {
        auto guard = buffer_pool_->fetch_page(table_name_, current);
        if (!guard.is_valid())
            return INVALID_PAGE_ID;

        const auto * hdr = reinterpret_cast<const PageHeader *>(guard.data());
        auto pt = static_cast<PageType>(hdr->page_type);
        if (pt == PageType::BPTREE_LEAF_PAGE || pt == PageType::DATA_PAGE)
        {
            return current;
        }
        if (pt == PageType::BPTREE_INTERNAL_PAGE)
        {
            const auto * internal = reinterpret_cast<const BTreeInternalPage *>(guard.data());
            current = internal->child_at(0);
        }
        else
        {
            return INVALID_PAGE_ID;
        }
    }
}

// --- B+Tree Insert ---

void TableHeap::insert_internal_with_path(
    const char * key_data, uint16_t key_len, page_id_t child_page, std::vector<page_id_t> & path, size_t path_idx)
{
    // path[path_idx] is the internal page to insert into
    page_id_t page_id = path[path_idx];
    auto guard = buffer_pool_->fetch_page(table_name_, page_id);
    if (!guard.is_valid())
        return;

    auto * internal = reinterpret_cast<BTreeInternalPage *>(guard.data());

    auto cmp = [this](const char * a, uint16_t al, const char * b, uint16_t bl) -> int
    {
        auto ka = extract_pk_from_serialized(a, al);
        auto kb = extract_pk_from_serialized(b, bl);
        return compare_pk(ka, kb);
    };

    // Try simple insert first
    if (internal->insert(key_data, key_len, child_page, cmp))
    {
        guard.mark_dirty();
        return;
    }

    // Page full — split it
    page_id_t new_pid = 0;
    auto new_guard = buffer_pool_->new_page(table_name_, new_pid);
    if (!new_guard.is_valid())
        return;

    auto * new_internal = reinterpret_cast<BTreeInternalPage *>(new_guard.data());
    new_internal->init(INVALID_PAGE_ID, internal->level());

    auto [promoted_key, _] = internal->split(new_internal);
    new_internal->set_page_id(new_pid);

    // Determine where the new (key_data, child_page) should go
    auto promoted_pk = extract_pk_from_serialized(promoted_key.data(), static_cast<uint16_t>(promoted_key.size()));
    auto insert_pk = extract_pk_from_serialized(key_data, key_len);
    int comp = compare_pk(insert_pk, promoted_pk);

    if (comp < 0)
    {
        internal->insert(key_data, key_len, child_page, cmp);
    }
    else
    {
        new_internal->insert(key_data, key_len, child_page, cmp);
    }

    guard.mark_dirty();
    new_guard.mark_dirty();

    // Propagate split upward
    if (path_idx == 0)
    {
        // This page IS the root — create a new root above
        page_id_t new_root_pid = 0;
        auto root_guard = buffer_pool_->new_page(table_name_, new_root_pid);
        if (!root_guard.is_valid())
            return;

        auto * new_root = reinterpret_cast<BTreeInternalPage *>(root_guard.data());
        new_root->init(new_root_pid, internal->level() + 1);

        // Set up new root: leftmost=page_id (old root), promoted_key -> new_pid
        new_root->set_rightmost_child(page_id);
        new_root->insert(promoted_key.data(), static_cast<uint16_t>(promoted_key.size()), new_pid, cmp);

        root_page_id_ = new_root_pid;
        root_guard.mark_dirty();
    }
    else
    {
        // Propagate to parent: path[path_idx - 1]
        insert_internal_with_path(promoted_key.data(), static_cast<uint16_t>(promoted_key.size()), new_pid, path, path_idx - 1);
    }
}

slot_id_t TableHeap::btree_insert(const char * data, uint16_t len, const neon::type::Value & pk)
{
    // If no root, create first leaf as root
    if (root_page_id_ == INVALID_PAGE_ID)
    {
        page_id_t new_pid = 0;
        auto guard = buffer_pool_->new_page(table_name_, new_pid);
        if (!guard.is_valid())
            return 0;

        auto * leaf = reinterpret_cast<DataPage *>(guard.data());
        leaf->init_as_leaf(new_pid);
        root_page_id_ = new_pid;
        guard.mark_dirty();
    }

    // Search for the target leaf, tracking path of internal nodes
    std::vector<page_id_t> path;
    page_id_t leaf_pid = search_leaf_with_path(pk, path);
    if (leaf_pid == INVALID_PAGE_ID)
    {
        spdlog::error("TableHeap::btree_insert: search_leaf returned invalid page");
        return 0;
    }

    auto leaf_guard = buffer_pool_->fetch_page(table_name_, leaf_pid);
    if (!leaf_guard.is_valid())
        return 0;

    auto * leaf = reinterpret_cast<DataPage *>(leaf_guard.data());

    // Comparator for sorted insert: compare by PK
    auto cmp = [this](const char * a_data, uint16_t a_len, const char * b_data, uint16_t b_len) -> int
    {
        auto ka = extract_pk_from_serialized(a_data, a_len);
        auto kb = extract_pk_from_serialized(b_data, b_len);
        return compare_pk(ka, kb);
    };

    // Try to insert into the leaf
    auto slot_opt = leaf->insert_tuple_sorted(data, len, cmp);
    if (slot_opt.has_value())
    {
        leaf_guard.mark_dirty();
        return slot_opt.value();
    }

    // Leaf is full — split it
    // 1. Create a new leaf page
    page_id_t new_leaf_pid = 0;
    auto new_leaf_guard = buffer_pool_->new_page(table_name_, new_leaf_pid);
    if (!new_leaf_guard.is_valid())
        return 0;

    auto * new_leaf = reinterpret_cast<DataPage *>(new_leaf_guard.data());
    new_leaf->init_as_leaf(new_leaf_pid);

    // 2. Link into leaf chain
    new_leaf->set_next_leaf_page(leaf->next_leaf_page());
    leaf->set_next_leaf_page(new_leaf_pid);

    // 3. Move latter half entries to new page
    leaf->split_leaf_entries(new_leaf);

    // 3.5 Compact old leaf to reclaim space from DELETED slots
    leaf->compact();

    // 4. Insert the new row into the correct page
    auto new_pk = extract_pk_from_serialized(data, len);
    const char * first_new_data = nullptr;
    uint16_t first_new_len = 0;
    for (uint16_t i = 0; i < new_leaf->slot_count(); i++)
    {
        auto [ptr, l] = new_leaf->get_tuple(i);
        if (ptr)
        {
            first_new_data = ptr;
            first_new_len = l;
            break;
        }
    }

    slot_id_t result_slot;
    if (first_new_data && compare_pk(new_pk, extract_pk_from_serialized(first_new_data, first_new_len)) < 0)
    {
        // New row belongs in old leaf
        result_slot = leaf->insert_tuple_sorted(data, len, cmp).value_or(0);
    }
    else
    {
        // New row belongs in new leaf
        result_slot = new_leaf->insert_tuple_sorted(data, len, cmp).value_or(0);
    }

    leaf_guard.mark_dirty();
    new_leaf_guard.mark_dirty();

    // 5. Propagate split upward — use first row in new leaf as promoted key
    const char * promoted_data = nullptr;
    uint16_t promoted_len = 0;
    for (uint16_t i = 0; i < new_leaf->slot_count(); i++)
    {
        auto [ptr, l] = new_leaf->get_tuple(i);
        if (ptr)
        {
            promoted_data = ptr;
            promoted_len = l;
            break;
        }
    }

    if (promoted_data)
    {
        if (path.empty())
        {
            // Root is a leaf — create a new root internal node
            page_id_t new_root_pid = 0;
            auto root_guard = buffer_pool_->new_page(table_name_, new_root_pid);
            if (!root_guard.is_valid())
                return result_slot;

            auto * new_root = reinterpret_cast<BTreeInternalPage *>(root_guard.data());
            new_root->init(new_root_pid, 0);
            // insert semantics: child_page is right child of key.
            // We want: leftmost=old_leaf, rightmost=new_leaf.
            new_root->set_rightmost_child(leaf_pid);

            auto cmp_internal = [this](const char * a, uint16_t al, const char * b, uint16_t bl) -> int
            {
                auto ka = extract_pk_from_serialized(a, al);
                auto kb = extract_pk_from_serialized(b, bl);
                return compare_pk(ka, kb);
            };
            new_root->insert(promoted_data, promoted_len, new_leaf_pid, cmp_internal);

            root_page_id_ = new_root_pid;
            root_guard.mark_dirty();
        }
        else
        {
            // path.back() is the direct parent of this leaf
            // Insert promoted key into parent
            insert_internal_with_path(promoted_data, promoted_len, new_leaf_pid, path, path.size() - 1);
        }
    }

    return result_slot;
}

// --- Public API ---

RowId TableHeap::insert_row(neon::txn::Transaction * txn, const Tuple & tuple)
{
    const auto & serialized = tuple.bytes();
    auto data_len = tuple.size();
    auto pk = extract_pk(tuple);

    std::lock_guard<std::mutex> lock(mutex_);

    page_id_t leaf_pid = INVALID_PAGE_ID;

    // Perform B+Tree insert
    // We need to know which page the insert ended up on.
    // For simplicity, search for the leaf after root is ensured.
    if (root_page_id_ == INVALID_PAGE_ID)
    {
        // Create root leaf
        page_id_t new_pid = 0;
        auto guard = buffer_pool_->new_page(table_name_, new_pid);
        if (!guard.is_valid())
        {
            return RowId{INVALID_PAGE_ID, 0};
        }
        auto * leaf = reinterpret_cast<DataPage *>(guard.data());
        leaf->init_as_leaf(new_pid);
        root_page_id_ = new_pid;
        guard.mark_dirty();
    }

    slot_id_t slot = btree_insert(serialized.data(), data_len, pk);
    std::vector<page_id_t> dummy_path;
    leaf_pid = search_leaf_with_path(pk, dummy_path);
    if (slot == 0 && leaf_pid == INVALID_PAGE_ID)
    {
        return RowId{INVALID_PAGE_ID, 0};
    }

    // Write redo log
    LogRecord record;
    record.txn_id = txn ? txn->txn_id() : INVALID_TXN_ID;
    record.type = LogType::INSERT;
    record.table_name = table_name_;
    record.page_id = leaf_pid;
    record.slot_id = slot;
    record.new_data = serialized;

    lsn_t lsn = redo_log_->append(record);

    // Update page LSN
    {
        auto guard = buffer_pool_->fetch_page(table_name_, leaf_pid);
        if (guard.is_valid())
        {
            auto * page = reinterpret_cast<DataPage *>(guard.data());
            page->header()->lsn = lsn;
            guard.mark_dirty();
        }
    }

    if (txn)
    {
        record.lsn = lsn;
        txn->add_to_write_set(record);
    }

    return RowId{leaf_pid, slot};
}

bool TableHeap::delete_row(neon::txn::Transaction * txn, RowId rid)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto guard = buffer_pool_->fetch_page(table_name_, rid.page_id);
    if (!guard.is_valid())
    {
        return false;
    }

    auto * page = reinterpret_cast<DataPage *>(guard.data());
    auto [old_data_ptr, old_len] = page->get_tuple(rid.slot_id);
    if (old_data_ptr == nullptr)
    {
        return false;
    }

    std::vector<char> old_data(old_data_ptr, old_data_ptr + old_len);

    if (!page->delete_tuple(rid.slot_id))
    {
        return false;
    }

    // Write redo log
    LogRecord record;
    record.txn_id = txn ? txn->txn_id() : INVALID_TXN_ID;
    record.type = LogType::DELETE;
    record.table_name = table_name_;
    record.page_id = rid.page_id;
    record.slot_id = rid.slot_id;
    record.old_data = std::move(old_data);

    lsn_t lsn = redo_log_->append(record);
    page->header()->lsn = lsn;
    guard.mark_dirty();

    if (txn)
    {
        record.lsn = lsn;
        txn->add_to_write_set(record);
    }

    return true;
}

RowId TableHeap::update_row(neon::txn::Transaction * txn, RowId rid, const Tuple & tuple)
{
    const auto & serialized = tuple.bytes();
    auto data_len = tuple.size();
    auto new_pk = extract_pk(tuple);

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<char> old_data;
    neon::type::Value old_pk;
    {
        auto guard = buffer_pool_->fetch_page(table_name_, rid.page_id);
        if (!guard.is_valid())
        {
            return RowId{INVALID_PAGE_ID, 0};
        }
        auto * page = reinterpret_cast<DataPage *>(guard.data());
        auto [old_data_ptr, old_len] = page->get_tuple(rid.slot_id);
        if (old_data_ptr == nullptr)
        {
            return RowId{INVALID_PAGE_ID, 0};
        }
        old_data.assign(old_data_ptr, old_data_ptr + old_len);
        old_pk = extract_pk_from_serialized(old_data_ptr, old_len);
    }

    // One generation covers the new page and the tombstone of the old page.
    // Abort reloads committed page images; the root id is the only pointer that
    // lives outside a page, so it is restored explicitly.
    DiskManager * disk = buffer_pool_->disk_manager();
    bool own_txn = disk->begin_txn();
    page_id_t root_snap = root_page_id_;
    struct CommitGuard
    {
        DiskManager * disk = nullptr;
        BufferPoolManager * pool = nullptr;
        page_id_t * root = nullptr;
        page_id_t root_snap = INVALID_PAGE_ID;
        bool own = false;
        bool ok = false;

        CommitGuard(DiskManager * d, BufferPoolManager * p, page_id_t * r, page_id_t snap, bool own_txn)
            : disk(d), pool(p), root(r), root_snap(snap), own(own_txn)
        {
        }

        ~CommitGuard()
        {
            if (!own)
            {
                return;
            }
            if (ok)
            {
                pool->flush_all();
                disk->commit_txn();
            }
            else
            {
                disk->abort_txn();
                pool->invalidate_all();
                *root = root_snap;
            }
        }
    } commit_guard(disk, buffer_pool_, &root_page_id_, root_snap, own_txn);

    // Same primary key and the new tuple fits in the slot: keep the logical page.
    // Flush still appends a new compressed image; the old image becomes garbage.
    if (compare_pk(old_pk, new_pk) == 0)
    {
        auto guard = buffer_pool_->fetch_page(table_name_, rid.page_id);
        if (guard.is_valid())
        {
            auto * page = reinterpret_cast<DataPage *>(guard.data());
            if (auto updated = page->update_tuple(rid.slot_id, serialized.data(), data_len); updated.has_value())
            {
                LogRecord record;
                record.txn_id = txn ? txn->txn_id() : INVALID_TXN_ID;
                record.type = LogType::UPDATE;
                record.table_name = table_name_;
                record.page_id = rid.page_id;
                record.slot_id = updated.value();
                record.old_data = old_data;
                record.new_data = serialized;
                lsn_t lsn = redo_log_->append(record);
                page->header()->lsn = lsn;
                guard.mark_dirty();
                if (txn)
                {
                    record.lsn = lsn;
                    txn->add_to_write_set(record);
                }
                commit_guard.ok = true;
                return RowId{rid.page_id, updated.value()};
            }
        }
    }

    char image[PAGE_SIZE];
    {
        auto guard = buffer_pool_->fetch_page(table_name_, rid.page_id);
        if (!guard.is_valid())
        {
            return RowId{INVALID_PAGE_ID, 0};
        }
        std::memcpy(image, guard.data(), PAGE_SIZE);
    }

    std::vector<page_id_t> path;
    page_id_t found = search_leaf_with_path(old_pk, path);
    if (found != rid.page_id)
    {
        return RowId{INVALID_PAGE_ID, 0};
    }

    page_id_t prev_leaf = INVALID_PAGE_ID;
    {
        page_id_t cur = find_leftmost_leaf();
        while (cur != INVALID_PAGE_ID && cur != rid.page_id)
        {
            auto walk = buffer_pool_->fetch_page(table_name_, cur);
            if (!walk.is_valid())
            {
                return RowId{INVALID_PAGE_ID, 0};
            }
            prev_leaf = cur;
            cur = reinterpret_cast<DataPage *>(walk.data())->next_leaf_page();
        }
    }

    page_id_t new_pid = INVALID_PAGE_ID;
    {
        auto new_guard = buffer_pool_->new_page(table_name_, new_pid);
        if (!new_guard.is_valid())
        {
            return RowId{INVALID_PAGE_ID, 0};
        }
        std::memcpy(new_guard.data(), image, PAGE_SIZE);
        auto * copied = reinterpret_cast<DataPage *>(new_guard.data());
        copied->header()->page_id = new_pid;

        bool rewritten = false;
        slot_id_t new_slot = rid.slot_id;
        if (compare_pk(old_pk, new_pk) == 0)
        {
            if (auto updated = copied->update_tuple(rid.slot_id, serialized.data(), data_len); updated.has_value())
            {
                new_slot = updated.value();
                rewritten = true;
            }
        }
        if (!rewritten)
        {
            copied->delete_tuple(rid.slot_id);
            copied->compact();
        }
        new_guard.mark_dirty();

        if (prev_leaf != INVALID_PAGE_ID)
        {
            auto prev_guard = buffer_pool_->fetch_page(table_name_, prev_leaf);
            if (prev_guard.is_valid())
            {
                reinterpret_cast<DataPage *>(prev_guard.data())->set_next_leaf_page(new_pid);
                prev_guard.mark_dirty();
            }
        }
        if (!path.empty())
        {
            auto parent_guard = buffer_pool_->fetch_page(table_name_, path.back());
            if (!parent_guard.is_valid()
                || !reinterpret_cast<BTreeInternalPage *>(parent_guard.data())->replace_child(rid.page_id, new_pid))
            {
                return RowId{INVALID_PAGE_ID, 0};
            }
            parent_guard.mark_dirty();
        }
        if (root_page_id_ == rid.page_id)
        {
            root_page_id_ = new_pid;
        }

        buffer_pool_->drop_page(table_name_, rid.page_id);
        disk->retire_page(table_name_, rid.page_id);

        if (!rewritten)
        {
            new_slot = btree_insert(serialized.data(), data_len, new_pk);
            std::vector<page_id_t> placed;
            new_pid = search_leaf_with_path(new_pk, placed);
        }

        LogRecord record;
        record.txn_id = txn ? txn->txn_id() : INVALID_TXN_ID;
        record.type = LogType::UPDATE;
        record.table_name = table_name_;
        record.page_id = new_pid;
        record.slot_id = new_slot;
        record.old_data = std::move(old_data);
        record.new_data = serialized;
        lsn_t lsn = redo_log_->append(record);
        {
            auto placed_guard = buffer_pool_->fetch_page(table_name_, new_pid);
            if (placed_guard.is_valid())
            {
                reinterpret_cast<DataPage *>(placed_guard.data())->header()->lsn = lsn;
                placed_guard.mark_dirty();
            }
        }
        if (txn)
        {
            record.lsn = lsn;
            txn->add_to_write_set(record);
        }
        commit_guard.ok = true;
        return RowId{new_pid, new_slot};
    }
}

std::optional<Tuple> TableHeap::get_row(RowId rid) const
{
    auto guard = buffer_pool_->fetch_page(table_name_, rid.page_id);
    if (!guard.is_valid())
    {
        return std::nullopt;
    }

    const auto * page = reinterpret_cast<const DataPage *>(guard.data());
    auto [data_ptr, len] = page->get_tuple(rid.slot_id);
    if (data_ptr == nullptr)
    {
        return std::nullopt;
    }

    return Tuple(std::vector<char>(data_ptr, data_ptr + len));
}

std::optional<RowId> TableHeap::lookup_pk(const neon::type::Value & pk) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (root_page_id_ == INVALID_PAGE_ID)
    {
        return std::nullopt;
    }

    std::vector<page_id_t> path;
    page_id_t leaf_pid = search_leaf_with_path(pk, path);
    if (leaf_pid == INVALID_PAGE_ID)
    {
        return std::nullopt;
    }

    auto guard = buffer_pool_->fetch_page(table_name_, leaf_pid);
    if (!guard.is_valid())
    {
        return std::nullopt;
    }

    const auto * page = reinterpret_cast<const DataPage *>(guard.data());

    // Binary search within leaf page (slots are PK-sorted).
    // Fall back to linear scan if deleted slots are encountered.
    uint16_t lo = 0;
    uint16_t hi = page->slot_count();
    bool use_linear = false;

    while (lo < hi && !use_linear)
    {
        uint16_t mid = lo + (hi - lo) / 2;
        auto [data_ptr, data_len] = page->get_tuple(mid);
        if (data_ptr == nullptr)
        {
            // Deleted slot encountered — fall back to linear scan
            use_linear = true;
            break;
        }
        auto mid_pk = extract_pk_from_serialized(data_ptr, data_len);
        int cmp = compare_pk(mid_pk, pk);
        if (cmp < 0)
        {
            lo = mid + 1;
        }
        else if (cmp > 0)
        {
            hi = mid;
        }
        else
        {
            return RowId{leaf_pid, mid};
        }
    }

    // Linear scan: check every valid slot
    for (uint16_t i = 0; i < page->slot_count(); i++)
    {
        auto [data_ptr, data_len] = page->get_tuple(i);
        if (data_ptr == nullptr)
            continue;
        auto slot_pk = extract_pk_from_serialized(data_ptr, data_len);
        if (compare_pk(slot_pk, pk) == 0)
        {
            return RowId{leaf_pid, i};
        }
    }

    return std::nullopt;
}

TableIterator TableHeap::iter() const
{
    auto it = TableIterator(const_cast<TableHeap *>(this), schema_);
    it.rewind();
    return it;
}

// ============================================================================
// TableIterator — B+Tree leaf page chain traversal
// ============================================================================

TableIterator::TableIterator(TableHeap * heap, const neon::catalog::Schema & schema) : heap_(heap), schema_(schema)
{
}

void TableIterator::rewind()
{
    page_id_t first = heap_->find_leftmost_leaf();
    if (first == INVALID_PAGE_ID)
    {
        valid_ = false;
        return;
    }
    current_page_id_ = first;
    current_slot_id_ = 0;
    valid_ = true;
    advance_to_valid();
}

bool TableIterator::next()
{
    if (!valid_)
    {
        return false;
    }
    current_slot_id_++;
    advance_to_valid();
    return valid_;
}

bool TableIterator::valid() const
{
    return valid_;
}

Tuple TableIterator::current_tuple() const
{
    auto guard = heap_->buffer_pool()->fetch_page(heap_->table_name(), current_page_id_);
    const auto * page = reinterpret_cast<const DataPage *>(guard.data());
    auto [data_ptr, len] = page->get_tuple(current_slot_id_);
    if (data_ptr == nullptr)
    {
        return Tuple();
    }
    return Tuple(std::vector<char>(data_ptr, data_ptr + len));
}

neon::type::Row TableIterator::current_row() const
{
    return current_tuple().deform(schema_);
}

RowId TableIterator::current_row_id() const
{
    return RowId{current_page_id_, current_slot_id_};
}

void TableIterator::advance_to_valid()
{
    while (true)
    {
        if (current_page_id_ == INVALID_PAGE_ID)
        {
            valid_ = false;
            return;
        }

        auto guard = heap_->buffer_pool()->fetch_page(heap_->table_name(), current_page_id_);
        if (!guard.is_valid())
        {
            valid_ = false;
            return;
        }

        const auto * page = reinterpret_cast<const DataPage *>(guard.data());
        uint16_t slot_cnt = page->slot_count();

        while (current_slot_id_ < slot_cnt)
        {
            auto [data_ptr, len] = page->get_tuple(current_slot_id_);
            if (data_ptr != nullptr)
            {
                valid_ = true;
                return;
            }
            current_slot_id_++;
        }

        // Move to next leaf page
        current_page_id_ = page->next_leaf_page();
        current_slot_id_ = 0;
    }
}

} // namespace neon::storage
