#include "src/storage/table_heap.h"
#include <algorithm>
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

uint64_t fold_value(const neon::type::Value & v)
{
    uint64_t x = static_cast<uint64_t>(v.datatype()) << 56;
    if (v.datatype() == neon::type::ValueType::String)
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : v.string_ref())
        {
            h ^= c;
            h *= 1099511628211ull;
        }
        x ^= h;
    }
    else if (v.datatype() != neon::type::ValueType::Null)
    {
        x ^= static_cast<uint64_t>(read_i64(v));
    }
    return x * 0x9E3779B185EBCA87ull;
}

neon::type::Value coerce_to(neon::type::ValueType type, const neon::type::Value & v)
{
    using neon::type::ValueType;
    if (v.datatype() == type || v.datatype() == ValueType::Null || type == ValueType::String)
    {
        return v;
    }
    int64_t n = read_i64(v);
    switch (type)
    {
        case ValueType::Int8:
            return neon::type::Value(type, static_cast<int8_t>(n));
        case ValueType::Int16:
            return neon::type::Value(type, static_cast<int16_t>(n));
        case ValueType::Int32:
            return neon::type::Value(type, static_cast<int32_t>(n));
        case ValueType::Int64:
            return neon::type::Value(type, n);
        case ValueType::Uint8:
            return neon::type::Value(type, static_cast<uint8_t>(n));
        case ValueType::Uint16:
            return neon::type::Value(type, static_cast<uint16_t>(n));
        case ValueType::Uint32:
            return neon::type::Value(type, static_cast<uint32_t>(n));
        case ValueType::Uint64:
            return neon::type::Value(type, static_cast<uint64_t>(n));
        default:
            return v;
    }
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

TableHeap::~TableHeap() = default;

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

int TableHeap::compare_tuple_pk(const char * a, uint16_t a_len, const char * b, uint16_t b_len) const
{
    if (sort_width_ > 1)
    {
        return compare_prefix(a, a_len, b, b_len, sort_width_);
    }
    int32_t pk_idx = schema_.primary_key_col_idx_;
    if (pk_idx != 0 || schema_.columns_.empty() || a_len < sizeof(uint16_t) || b_len < sizeof(uint16_t))
    {
        return compare_pk(extract_pk_from_serialized(a, a_len), extract_pk_from_serialized(b, b_len));
    }

    auto read_i64 = [this](const char * data, uint16_t len, bool & is_null, int64_t & out) -> bool
    {
        uint16_t num_cols = 0;
        std::memcpy(&num_cols, data, sizeof(uint16_t));
        uint32_t bitmap_bytes = (static_cast<uint32_t>(num_cols) + 7) / 8;
        if (len < sizeof(uint16_t) + bitmap_bytes)
        {
            return false;
        }
        is_null = (static_cast<unsigned char>(data[sizeof(uint16_t)]) & 1) != 0;
        if (is_null)
        {
            return true;
        }
        const char * payload = data + sizeof(uint16_t) + bitmap_bytes;
        uint32_t remain = len - (sizeof(uint16_t) + bitmap_bytes);
        switch (schema_.columns_[0].data_type_)
        {
            case neon::type::ValueType::Int8:
                if (remain < 1)
                    return false;
                out = static_cast<int8_t>(payload[0]);
                return true;
            case neon::type::ValueType::Uint8:
            case neon::type::ValueType::Char:
            case neon::type::ValueType::Bool:
                if (remain < 1)
                    return false;
                out = static_cast<unsigned char>(payload[0]);
                return true;
            case neon::type::ValueType::Int16: {
                if (remain < 2)
                    return false;
                int16_t v = 0;
                std::memcpy(&v, payload, 2);
                out = v;
                return true;
            }
            case neon::type::ValueType::Uint16: {
                if (remain < 2)
                    return false;
                uint16_t v = 0;
                std::memcpy(&v, payload, 2);
                out = v;
                return true;
            }
            case neon::type::ValueType::Int32: {
                if (remain < 4)
                    return false;
                int32_t v = 0;
                std::memcpy(&v, payload, 4);
                out = v;
                return true;
            }
            case neon::type::ValueType::Uint32: {
                if (remain < 4)
                    return false;
                uint32_t v = 0;
                std::memcpy(&v, payload, 4);
                out = v;
                return true;
            }
            case neon::type::ValueType::Int64:
            case neon::type::ValueType::Uint64: {
                if (remain < 8)
                    return false;
                std::memcpy(&out, payload, 8);
                return true;
            }
            default:
                return false;
        }
    };

    bool a_null = false;
    bool b_null = false;
    int64_t a_val = 0;
    int64_t b_val = 0;
    if (!read_i64(a, a_len, a_null, a_val) || !read_i64(b, b_len, b_null, b_val))
    {
        return compare_pk(extract_pk_from_serialized(a, a_len), extract_pk_from_serialized(b, b_len));
    }
    if (a_null && b_null)
        return 0;
    if (a_null)
        return -1;
    if (b_null)
        return 1;
    if (a_val < b_val)
        return -1;
    if (a_val > b_val)
        return 1;
    return 0;
}

// --- B+Tree Navigation ---

page_id_t TableHeap::search_leaf_bytes(const char * data, uint16_t len, std::vector<page_id_t> & path) const
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
            return current;
        }
        if (page_type == PageType::BPTREE_INTERNAL_PAGE)
        {
            path.push_back(current);
            const auto * internal = reinterpret_cast<const BTreeInternalPage *>(guard.data());
            auto cmp = [this](const char * a_data, uint16_t a_len, const char * b_data, uint16_t b_len) -> int
            { return compare_tuple_pk(a_data, a_len, b_data, b_len); };
            current = internal->find_child(data, len, cmp);
        }
        else
        {
            return INVALID_PAGE_ID;
        }
    }
}

page_id_t TableHeap::search_leaf_with_path(const neon::type::Value & pk, std::vector<page_id_t> & path) const
{
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
    return search_leaf_bytes(key.data(), key.size(), path);
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
    { return compare_tuple_pk(a, al, b, bl); };

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
    int comp = compare_tuple_pk(key_data, key_len, promoted_key.data(), static_cast<uint16_t>(promoted_key.size()));

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

std::pair<slot_id_t, page_id_t> TableHeap::btree_insert(const char * data, uint16_t len, const neon::type::Value & pk)
{
    // If no root, create first leaf as root
    if (root_page_id_ == INVALID_PAGE_ID)
    {
        page_id_t new_pid = 0;
        auto guard = buffer_pool_->new_page(table_name_, new_pid);
        if (!guard.is_valid())
            return {0, INVALID_PAGE_ID};

        auto * leaf = reinterpret_cast<DataPage *>(guard.data());
        leaf->init_as_leaf(new_pid);
        root_page_id_ = new_pid;
        guard.mark_dirty();
    }

    // Search for the target leaf, tracking path of internal nodes.
    // Composite index keys must be sought as the whole tuple, not column 0 alone.
    std::vector<page_id_t> path;
    page_id_t leaf_pid = sort_width_ > 1 ? search_leaf_bytes(data, len, path) : search_leaf_with_path(pk, path);
    if (leaf_pid == INVALID_PAGE_ID)
    {
        spdlog::error("TableHeap::btree_insert: search_leaf returned invalid page");
        return {0, INVALID_PAGE_ID};
    }

    auto leaf_guard = buffer_pool_->fetch_page(table_name_, leaf_pid);
    if (!leaf_guard.is_valid())
        return {0, INVALID_PAGE_ID};

    auto * leaf = reinterpret_cast<DataPage *>(leaf_guard.data());

    // Comparator for sorted insert: compare by PK
    auto cmp = [this](const char * a_data, uint16_t a_len, const char * b_data, uint16_t b_len) -> int
    { return compare_tuple_pk(a_data, a_len, b_data, b_len); };

    // Try to insert into the leaf
    auto slot_opt = leaf->insert_tuple_sorted(data, len, cmp);
    if (slot_opt.has_value())
    {
        leaf_guard.mark_dirty();
        return {slot_opt.value(), leaf_pid};
    }

    // Leaf is full — split it
    // 1. Create a new leaf page
    page_id_t new_leaf_pid = 0;
    auto new_leaf_guard = buffer_pool_->new_page(table_name_, new_leaf_pid);
    if (!new_leaf_guard.is_valid())
        return {0, INVALID_PAGE_ID};

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
    page_id_t dest_pid = new_leaf_pid;
    if (first_new_data && compare_tuple_pk(data, len, first_new_data, first_new_len) < 0)
    {
        // New row belongs in old leaf
        result_slot = leaf->insert_tuple_sorted(data, len, cmp).value_or(0);
        dest_pid = leaf_pid;
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
                return {result_slot, dest_pid};

            auto * new_root = reinterpret_cast<BTreeInternalPage *>(root_guard.data());
            new_root->init(new_root_pid, 0);
            // insert semantics: child_page is right child of key.
            // We want: leftmost=old_leaf, rightmost=new_leaf.
            new_root->set_rightmost_child(leaf_pid);

            auto cmp_internal = [this](const char * a, uint16_t al, const char * b, uint16_t bl) -> int
            { return compare_tuple_pk(a, al, b, bl); };
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

    return {result_slot, dest_pid};
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

    auto placed = btree_insert(serialized.data(), data_len, pk);
    slot_id_t slot = placed.first;
    leaf_pid = placed.second;
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

    if (!indexes_.empty() && !sync_indexes_insert(tuple, pk))
    {
        auto undo = buffer_pool_->fetch_page(table_name_, leaf_pid);
        if (undo.is_valid())
        {
            reinterpret_cast<DataPage *>(undo.data())->delete_tuple(slot);
            undo.mark_dirty();
        }
        ahi_.clear();
        return RowId{INVALID_PAGE_ID, 0};
    }
    ahi_.clear();
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

    sync_indexes_delete(record.old_data.data(), static_cast<uint16_t>(record.old_data.size()));
    ahi_.clear();
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
    std::vector<page_id_t> index_snap = index_roots();
    struct CommitGuard
    {
        DiskManager * disk = nullptr;
        BufferPoolManager * pool = nullptr;
        TableHeap * heap = nullptr;
        page_id_t * root = nullptr;
        page_id_t root_snap = INVALID_PAGE_ID;
        std::vector<page_id_t> index_snap;
        bool own = false;
        bool ok = false;

        CommitGuard(
            DiskManager * d, BufferPoolManager * p, TableHeap * heap, page_id_t * r, page_id_t snap, std::vector<page_id_t> indexes,
            bool own_txn)
            : disk(d), pool(p), heap(heap), root(r), root_snap(snap), index_snap(std::move(indexes)), own(own_txn)
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
                heap->set_index_roots(index_snap);
                heap->clear_ahi();
            }
        }
    } commit_guard(disk, buffer_pool_, this, &root_page_id_, root_snap, std::move(index_snap), own_txn);

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
                sync_indexes_update(old_data.data(), static_cast<uint16_t>(old_data.size()), tuple);
                ahi_.clear();
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
            auto placed = btree_insert(serialized.data(), data_len, new_pk);
            new_slot = placed.first;
            new_pid = placed.second;
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
        sync_indexes_update(record.old_data.data(), static_cast<uint16_t>(record.old_data.size()), tuple);
        ahi_.clear();
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

std::optional<neon::type::Value> TableHeap::read_column(RowId rid, int32_t column) const
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
    return RowSerializer::read_column(data_ptr, len, schema_, column);
}

std::optional<RowId> TableHeap::lookup_pk(const neon::type::Value & pk) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (root_page_id_ == INVALID_PAGE_ID)
    {
        return std::nullopt;
    }

    uint64_t fold = fold_value(pk);
    if (auto hashed = ahi_.find(fold))
    {
        auto hashed_guard = buffer_pool_->fetch_page(table_name_, hashed->page);
        if (hashed_guard.is_valid())
        {
            const auto * hashed_page = reinterpret_cast<const DataPage *>(hashed_guard.data());
            auto [hashed_data, hashed_len] = hashed_page->get_tuple(hashed->slot);
            if (hashed_data != nullptr && compare_pk(extract_pk_from_serialized(hashed_data, hashed_len), pk) == 0)
            {
                return RowId{hashed->page, hashed->slot};
            }
        }
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
            ahi_note(leaf_pid, mid, pk);
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
            ahi_note(leaf_pid, i, pk);
            return RowId{leaf_pid, i};
        }
    }

    return std::nullopt;
}

int64_t TableHeap::count_rows() const
{
    int64_t count = 0;
    page_id_t page_id = find_leftmost_leaf();
    while (page_id != INVALID_PAGE_ID)
    {
        auto guard = buffer_pool_->fetch_page(table_name_, page_id);
        if (!guard.is_valid())
        {
            break;
        }
        const auto * page = reinterpret_cast<const DataPage *>(guard.data());
        uint16_t slots = page->slot_count();
        for (uint16_t i = 0; i < slots; ++i)
        {
            auto [data_ptr, len] = page->get_tuple(i);
            if (data_ptr != nullptr)
            {
                count++;
            }
        }
        page_id = page->next_leaf_page();
    }
    return count;
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

void TableIterator::seek_ge(const neon::type::Value & key)
{
    std::vector<page_id_t> path;
    page_id_t leaf = heap_->search_leaf_with_path(key, path);
    if (leaf == INVALID_PAGE_ID)
    {
        valid_ = false;
        return;
    }
    current_page_id_ = leaf;
    page_guard_ = heap_->buffer_pool()->fetch_page(heap_->table_name(), leaf);
    if (!page_guard_.is_valid())
    {
        valid_ = false;
        return;
    }
    const auto * page = reinterpret_cast<const DataPage *>(page_guard_.data());
    uint16_t slot = page->slot_count();
    for (uint16_t i = 0; i < page->slot_count(); ++i)
    {
        auto [data_ptr, len] = page->get_tuple(i);
        if (data_ptr == nullptr)
        {
            continue;
        }
        auto slot_pk = heap_->extract_pk_from_serialized(data_ptr, len);
        if (heap_->compare_pk(slot_pk, key) >= 0)
        {
            slot = i;
            break;
        }
    }
    if (slot >= page->slot_count())
    {
        current_page_id_ = page->next_leaf_page();
        current_slot_id_ = 0;
        page_guard_ = PageGuard();
        valid_ = current_page_id_ != INVALID_PAGE_ID;
        if (valid_)
        {
            advance_to_valid();
        }
        return;
    }
    current_slot_id_ = slot;
    valid_ = true;
    advance_to_valid();
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
    const DataPage * page = nullptr;
    PageGuard fallback;
    if (page_guard_.is_valid())
    {
        page = reinterpret_cast<const DataPage *>(page_guard_.data());
    }
    else
    {
        fallback = heap_->buffer_pool()->fetch_page(heap_->table_name(), current_page_id_);
        if (!fallback.is_valid())
        {
            return Tuple();
        }
        page = reinterpret_cast<const DataPage *>(fallback.data());
    }
    auto [data_ptr, len] = page->get_tuple(current_slot_id_);
    if (data_ptr == nullptr)
    {
        return Tuple();
    }
    return Tuple(std::vector<char>(data_ptr, data_ptr + len));
}

std::pair<const char *, uint32_t> TableIterator::current_bytes() const
{
    if (!valid_ || !page_guard_.is_valid())
    {
        return {nullptr, 0};
    }
    const auto * page = reinterpret_cast<const DataPage *>(page_guard_.data());
    auto [data_ptr, len] = page->get_tuple(current_slot_id_);
    return {data_ptr, len};
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

        if (!page_guard_.is_valid() || page_guard_.page_id() != current_page_id_)
        {
            page_guard_ = heap_->buffer_pool()->fetch_page(heap_->table_name(), current_page_id_);
        }
        if (!page_guard_.is_valid())
        {
            valid_ = false;
            return;
        }

        const auto * page = reinterpret_cast<const DataPage *>(page_guard_.data());
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
        page_guard_ = PageGuard();
    }
}

int TableHeap::compare_prefix(const char * a, uint16_t a_len, const char * b, uint16_t b_len, int width) const
{
    auto left = RowSerializer::deserialize(a, a_len, schema_);
    auto right = RowSerializer::deserialize(b, b_len, schema_);
    int n = std::min(width, static_cast<int>(schema_.columns_.size()));
    for (int i = 0; i < n; ++i)
    {
        const neon::type::Value & av = i < static_cast<int>(left.values_.size()) ? left.values_[i] : neon::type::Value();
        const neon::type::Value & bv = i < static_cast<int>(right.values_.size()) ? right.values_[i] : neon::type::Value();
        int cmp = compare_as(schema_.columns_[static_cast<size_t>(i)].data_type_, av, bv);
        if (cmp != 0)
        {
            return cmp;
        }
    }
    return 0;
}

void TableHeap::ahi_build(page_id_t leaf) const
{
    auto guard = buffer_pool_->fetch_page(table_name_, leaf);
    if (!guard.is_valid())
    {
        return;
    }
    const auto * page = reinterpret_cast<const DataPage *>(guard.data());
    for (uint16_t i = 0; i < page->slot_count(); ++i)
    {
        auto [data_ptr, len] = page->get_tuple(i);
        if (data_ptr == nullptr)
        {
            continue;
        }
        auto pk = extract_pk_from_serialized(data_ptr, len);
        ahi_.remember(fold_value(pk), leaf, i);
    }
}

void TableHeap::ahi_note(page_id_t leaf, slot_id_t slot, const neon::type::Value & pk) const
{
    if (ahi_.observe(leaf))
    {
        ahi_build(leaf);
    }
    else if (ahi_.hot(leaf))
    {
        ahi_.remember(fold_value(pk), leaf, slot);
    }
}

void TableHeap::clear_ahi()
{
    ahi_.clear();
}

bool TableHeap::insert_entry(const neon::type::Value & key, const neon::type::Value & pk)
{
    if (schema_.columns_.size() < 2)
    {
        return false;
    }
    neon::type::Row row;
    row.append_value(coerce_to(schema_.columns_[0].data_type_, key));
    row.append_value(coerce_to(schema_.columns_[1].data_type_, pk));
    auto tuple = Tuple::form(schema_, row);
    return insert_row(nullptr, tuple).is_valid();
}

bool TableHeap::erase_entry(const neon::type::Value & key, const neon::type::Value & pk)
{
    if (schema_.columns_.size() < 2)
    {
        return false;
    }
    neon::type::Row row;
    row.append_value(coerce_to(schema_.columns_[0].data_type_, key));
    row.append_value(coerce_to(schema_.columns_[1].data_type_, pk));
    auto tuple = Tuple::form(schema_, row);

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<page_id_t> path;
    page_id_t leaf = search_leaf_bytes(tuple.data(), tuple.size(), path);
    if (leaf == INVALID_PAGE_ID)
    {
        return false;
    }
    auto guard = buffer_pool_->fetch_page(table_name_, leaf);
    if (!guard.is_valid())
    {
        return false;
    }
    auto * page = reinterpret_cast<DataPage *>(guard.data());
    for (uint16_t i = 0; i < page->slot_count(); ++i)
    {
        auto [data_ptr, len] = page->get_tuple(i);
        if (data_ptr == nullptr)
        {
            continue;
        }
        if (compare_tuple_pk(data_ptr, len, tuple.data(), tuple.size()) == 0)
        {
            page->delete_tuple(i);
            guard.mark_dirty();
            ahi_.clear();
            return true;
        }
    }
    return false;
}

std::vector<neon::type::Value> TableHeap::lookup_prefix(const neon::type::Value & key) const
{
    std::vector<neon::type::Value> found;
    if (schema_.columns_.empty() || root_page_id_ == INVALID_PAGE_ID)
    {
        return found;
    }
    neon::type::Row probe_row;
    probe_row.append_value(coerce_to(schema_.columns_[0].data_type_, key));
    probe_row.append_value(neon::type::Value());
    auto probe = Tuple::form(schema_, probe_row);

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<page_id_t> path;
    page_id_t leaf = search_leaf_bytes(probe.data(), probe.size(), path);
    auto key_type = schema_.columns_[0].data_type_;
    while (leaf != INVALID_PAGE_ID)
    {
        auto guard = buffer_pool_->fetch_page(table_name_, leaf);
        if (!guard.is_valid())
        {
            break;
        }
        const auto * page = reinterpret_cast<const DataPage *>(guard.data());
        bool passed = false;
        for (uint16_t i = 0; i < page->slot_count(); ++i)
        {
            auto [data_ptr, len] = page->get_tuple(i);
            if (data_ptr == nullptr)
            {
                continue;
            }
            if (compare_tuple_pk(data_ptr, len, probe.data(), probe.size()) < 0)
            {
                continue;
            }
            auto row = RowSerializer::deserialize(data_ptr, len, schema_);
            if (row.values_.empty())
            {
                continue;
            }
            int cmp = compare_as(key_type, row.values_[0], probe_row.values_[0]);
            if (cmp > 0)
            {
                passed = true;
                break;
            }
            if (cmp == 0 && row.values_.size() > 1)
            {
                found.push_back(row.values_[1]);
            }
        }
        if (passed)
        {
            break;
        }
        leaf = page->next_leaf_page();
    }
    return found;
}

void TableHeap::attach_index(std::string name, int32_t column, bool unique, std::unique_ptr<TableHeap> tree)
{
    if (schema_.primary_key_col_idx_ >= 0)
    {
        auto scan = iter();
        while (scan.valid())
        {
            auto row = scan.current_row();
            if (column >= 0 && column < static_cast<int32_t>(row.values_.size()))
            {
                tree->insert_entry(row.values_[static_cast<size_t>(column)], row.values_[static_cast<size_t>(schema_.primary_key_col_idx_)]);
            }
            scan.next();
        }
    }
    indexes_.push_back(SecondaryIndex{std::move(name), column, unique, std::move(tree)});
}

std::vector<neon::type::Value> TableHeap::lookup_index(size_t ordinal, const neon::type::Value & key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (ordinal >= indexes_.size() || !indexes_[ordinal].tree)
    {
        return {};
    }
    return indexes_[ordinal].tree->lookup_prefix(key);
}

std::vector<page_id_t> TableHeap::index_roots() const
{
    std::vector<page_id_t> roots;
    roots.reserve(indexes_.size());
    for (const auto & index : indexes_)
    {
        roots.push_back(index.tree ? index.tree->get_root_page_id() : INVALID_PAGE_ID);
    }
    return roots;
}

void TableHeap::set_index_roots(const std::vector<page_id_t> & roots)
{
    for (size_t i = 0; i < roots.size() && i < indexes_.size(); ++i)
    {
        if (indexes_[i].tree)
        {
            indexes_[i].tree->set_root_page_id(roots[i]);
            indexes_[i].tree->clear_ahi();
        }
    }
}

bool TableHeap::sync_indexes_insert(const Tuple & tuple, const neon::type::Value & pk)
{
    auto row = tuple.deform(schema_);
    std::vector<size_t> done;
    for (size_t i = 0; i < indexes_.size(); ++i)
    {
        auto & index = indexes_[i];
        if (index.column < 0 || index.column >= static_cast<int32_t>(row.values_.size()) || !index.tree)
        {
            continue;
        }
        const auto & key = row.values_[static_cast<size_t>(index.column)];
        if (index.unique && !index.tree->lookup_prefix(key).empty())
        {
            for (size_t j : done)
            {
                auto & prev = indexes_[j];
                prev.tree->erase_entry(row.values_[static_cast<size_t>(prev.column)], pk);
            }
            return false;
        }
        if (!index.tree->insert_entry(key, pk))
        {
            return false;
        }
        done.push_back(i);
    }
    return true;
}

void TableHeap::sync_indexes_delete(const char * data, uint16_t len)
{
    if (indexes_.empty() || schema_.primary_key_col_idx_ < 0)
    {
        return;
    }
    auto row = RowSerializer::deserialize(data, len, schema_);
    if (schema_.primary_key_col_idx_ >= static_cast<int32_t>(row.values_.size()))
    {
        return;
    }
    const auto & pk = row.values_[static_cast<size_t>(schema_.primary_key_col_idx_)];
    for (auto & index : indexes_)
    {
        if (!index.tree || index.column < 0 || index.column >= static_cast<int32_t>(row.values_.size()))
        {
            continue;
        }
        index.tree->erase_entry(row.values_[static_cast<size_t>(index.column)], pk);
    }
}

void TableHeap::sync_indexes_update(const char * old_data, uint16_t old_len, const Tuple & neu)
{
    sync_indexes_delete(old_data, old_len);
    sync_indexes_insert(neu, extract_pk(neu));
}

} // namespace neon::storage
