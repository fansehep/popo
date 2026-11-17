#include "src/trancation/mvcc.h"
#include <algorithm>
#include "src/catalog/schema.h"
#include "src/storage/row_serializer.h"
#include "src/storage/storage_engine.h"
#include "src/storage/table_heap.h"

namespace popo::txn
{

using popo::storage::LogRecord;
using popo::storage::LogType;
using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

struct MvccManager::Cursor::Impl
{
    using OverlayRows = std::map<Value, std::optional<Row>>;

    Impl(
        MvccManager * owner, Transaction * transaction, std::string table_name, std::optional<Value> lower,
        std::optional<Value> upper)
        : manager(owner), txn(transaction), table(std::move(table_name)), low(std::move(lower)), high(std::move(upper)),
          snapshot(manager->snapshot_csn_for(txn))
    {
        version_lock = std::shared_lock<std::shared_mutex>(manager->version_mutex_);
        if (auto found = manager->versions_.find(table); found != manager->versions_.end())
        {
            history = &found->second;
            history_it = low ? history->lower_bound(*low) : history->begin();
        }

        if (auto found = txn->overlay().find(table); found != txn->overlay().end())
        {
            own = &found->second.rows;
            own_it = low ? own->lower_bound(*low) : own->begin();
        }

        heap = manager->engine_->get_table_heap(table);
        if (heap != nullptr)
        {
            heap_it = std::make_unique<popo::storage::TableIterator>(heap->iter());
            if (low)
            {
                heap_it->seek_ge(*low);
            }
            load_heap();
        }
    }

    bool next(Row & out)
    {
        ValueLess less;
        while (true)
        {
            std::unique_lock<std::mutex> dirty_lock;
            if (txn->isolation() == IsolationLevel::ReadUncommitted)
            {
                dirty_lock = std::unique_lock<std::mutex>(manager->txn_mutex_);
            }
            std::optional<Value> key;
            auto consider = [&](const Value & candidate)
            {
                if (!key || less(candidate, *key))
                {
                    key = candidate;
                }
            };

            if (heap_row)
            {
                consider(heap_pk);
            }
            if (history != nullptr && history_it != history->end())
            {
                consider(history_it->first);
            }
            if (own != nullptr && own_it != own->end())
            {
                consider(own_it->first);
            }
            if (dirty_lock.owns_lock())
            {
                for (const auto & [id, other] : manager->active_)
                {
                    if (other.get() == txn)
                    {
                        continue;
                    }
                    auto found = other->overlay().find(table);
                    if (found == other->overlay().end())
                    {
                        continue;
                    }
                    auto it = after ? found->second.rows.upper_bound(*after)
                                    : (low ? found->second.rows.lower_bound(*low) : found->second.rows.begin());
                    if (it != found->second.rows.end())
                    {
                        consider(it->first);
                    }
                }
            }
            if (!key || (high && less(*high, *key)))
            {
                return false;
            }

            std::optional<Row> visible_row;
            bool resolved = false;

            // Current heap image is the baseline. Incremental history overrides
            // it for touched keys, including tombstones and "not yet inserted".
            if (heap_row && equal(heap_pk, *key))
            {
                visible_row = *heap_row;
                resolved = true;
            }
            if (history != nullptr && history_it != history->end() && equal(history_it->first, *key))
            {
                visible_row.reset();
                resolved = true;
                for (const auto & version : history_it->second)
                {
                    if (manager->visible(version, snapshot))
                    {
                        visible_row = version.row;
                        break;
                    }
                }
            }
            // Dirty overlays are above committed state; own writes win last.
            if (dirty_lock.owns_lock())
            {
                for (const auto & [id, other] : manager->active_)
                {
                    if (other.get() == txn)
                    {
                        continue;
                    }
                    auto found = other->overlay().find(table);
                    if (found != other->overlay().end())
                    {
                        auto row = found->second.rows.find(*key);
                        if (row != found->second.rows.end())
                        {
                            visible_row = row->second;
                            resolved = true;
                        }
                    }
                }
            }
            if (own != nullptr && own_it != own->end() && equal(own_it->first, *key))
            {
                visible_row = own_it->second;
                resolved = true;
            }

            advance_key(*key);
            after = *key;
            if (resolved && visible_row)
            {
                out = std::move(*visible_row);
                return true;
            }
        }
    }

    bool equal(const Value & a, const Value & b) const
    {
        ValueLess less;
        return !less(a, b) && !less(b, a);
    }

    void load_heap()
    {
        heap_row.reset();
        if (heap_it && heap_it->valid())
        {
            heap_row = heap_it->current_row();
            heap_pk = heap->schema().extract_pk(*heap_row);
        }
    }

    void advance_key(const Value & key)
    {
        if (heap_row && equal(heap_pk, key))
        {
            heap_it->next();
            load_heap();
        }
        if (history != nullptr && history_it != history->end() && equal(history_it->first, key))
        {
            ++history_it;
        }
        if (own != nullptr && own_it != own->end() && equal(own_it->first, key))
        {
            ++own_it;
        }
    }

    MvccManager * manager;
    Transaction * txn;
    std::string table;
    std::optional<Value> low;
    std::optional<Value> high;
    uint64_t snapshot;
    popo::storage::TableHeap * heap = nullptr;
    std::unique_ptr<popo::storage::TableIterator> heap_it;
    std::optional<Row> heap_row;
    Value heap_pk;
    const VersionedTable * history = nullptr;
    VersionedTable::const_iterator history_it;
    const OverlayRows * own = nullptr;
    OverlayRows::const_iterator own_it;
    std::optional<Value> after;
    std::shared_lock<std::shared_mutex> version_lock;
};

MvccManager::Cursor::Cursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) { }
MvccManager::Cursor::Cursor(Cursor &&) noexcept = default;
MvccManager::Cursor & MvccManager::Cursor::operator=(Cursor &&) noexcept = default;
MvccManager::Cursor::~Cursor() = default;
bool MvccManager::Cursor::next(Row & row) { return impl_ && impl_->next(row); }

MvccManager::MvccManager(popo::storage::StorageEngine * engine, popo::storage::RedoLogManager * redo_log)
    : engine_(engine), redo_log_(redo_log)
{
}

MvccManager::~MvccManager() = default;

bool MvccManager::ValueLess::operator()(const Value & a, const Value & b) const
{
    if (a.datatype() != b.datatype())
    {
        if (a.is_numeric() && b.is_numeric())
        {
            return a.to_f64() < b.to_f64();
        }
        return static_cast<int>(a.datatype()) < static_cast<int>(b.datatype());
    }
    if (a.datatype() == ValueType::Null)
    {
        return false;
    }
    return a < b;
}

popo::base::Result<Value> MvccManager::primary_key_of(const std::string & table, const Row & row) const
{
    auto * heap = engine_->get_table_heap(table);
    if (heap == nullptr)
    {
        return popo::base::make_error(popo::base::Errno::TableNotFound, "MVCC: table not found: " + table);
    }
    const auto & schema = heap->schema();
    if (schema.primary_key_col_idx_ >= 0 && static_cast<size_t>(schema.primary_key_col_idx_) >= row.values_.size())
    {
        return popo::base::make_error(popo::base::Errno::InvalidArg, "MVCC: row has no primary key");
    }
    return schema.extract_pk(row);
}

uint64_t MvccManager::snapshot_csn_for(Transaction * txn)
{
    switch (txn->isolation())
    {
        case IsolationLevel::ReadUncommitted:
            return UINT64_MAX; // see all committed data; dirty overlays handled separately
        case IsolationLevel::ReadCommitted:
            return txn->statement_csn() != 0 ? txn->statement_csn() : current_csn();
        case IsolationLevel::RepeatableRead:
        case IsolationLevel::Serializable:
            return txn->snapshot().csn;
    }
    return current_csn();
}

bool MvccManager::visible(const Version & v, uint64_t snapshot_csn) const
{
    // A snapshot holds the CSN the next commit will take, so it sees every
    // version created before that and still alive at it.
    return v.begin_csn < snapshot_csn && v.end_csn >= snapshot_csn;
}

Transaction * MvccManager::begin(IsolationLevel level)
{
    std::lock_guard<std::mutex> lock(txn_mutex_);
    auto id = next_txn_id_.fetch_add(1);
    auto txn = std::make_unique<Transaction>(id);
    txn->set_isolation(level);
    txn->set_snapshot(Snapshot{current_csn()});

    LogRecord rec;
    rec.txn_id = id;
    rec.type = LogType::BEGIN;
    rec.prev_lsn = popo::storage::INVALID_LSN;
    auto lsn = redo_log_->append(rec);
    txn->set_start_lsn(lsn);
    txn->set_last_lsn(lsn);

    Transaction * raw = txn.get();
    active_.emplace(id, std::move(txn));
    return raw;
}

Transaction * MvccManager::get_txn(popo::storage::txn_id_t id)
{
    std::lock_guard<std::mutex> lock(txn_mutex_);
    auto it = active_.find(id);
    return it != active_.end() ? it->second.get() : nullptr;
}

void MvccManager::begin_statement(Transaction * txn)
{
    txn->set_statement_csn(current_csn());
}

void MvccManager::log_operation(
    Transaction * txn, LogType type, const std::string & table, const std::vector<char> & old_data, const std::vector<char> & new_data)
{
    LogRecord rec;
    rec.txn_id = txn->txn_id();
    rec.type = type;
    rec.table_name = table;
    rec.prev_lsn = txn->last_lsn();
    rec.old_data = old_data;
    rec.new_data = new_data;
    auto lsn = redo_log_->append(rec);
    rec.lsn = lsn;
    txn->set_last_lsn(lsn);
    txn->add_to_write_set(rec);
}

popo::base::Result<void> MvccManager::stage_insert(Transaction * txn, const std::string & table, const Row & raw_row)
{
    auto * heap = engine_->get_table_heap(table);
    if (heap == nullptr)
    {
        return popo::base::make_error(popo::base::Errno::TableNotFound, "MVCC: table not found: " + table);
    }
    Row row = heap->schema().coerce_row(raw_row);
    auto pk = heap->schema().extract_pk(row);
    if (primary_key_taken(txn, table, pk))
    {
        return popo::base::make_error(popo::base::Errno::DuplicateKey, "Duplicate entry '" + pk.to_val_string() + "' for key 'PRIMARY'");
    }
    POPO_TRY(check_unique(txn, table, row));
    auto bytes = popo::storage::RowSerializer::serialize(row, heap->schema());
    log_operation(txn, LogType::INSERT, table, {}, bytes);
    publish_overlay(txn, table, pk, row);
    return {};
}

popo::base::Result<void> MvccManager::stage_update(Transaction * txn, const std::string & table, const Value & raw_pk, const Row & raw_row)
{
    auto * heap = engine_->get_table_heap(table);
    if (heap == nullptr)
    {
        return popo::base::make_error(popo::base::Errno::TableNotFound, "MVCC: table not found: " + table);
    }
    Row row = heap->schema().coerce_row(raw_row);
    Value pk = heap->schema().primary_key_col_idx_ >= 0
        ? popo::catalog::Schema::coerce_value(
              heap->schema().columns_[static_cast<size_t>(heap->schema().primary_key_col_idx_)].data_type_, raw_pk)
        : raw_pk;
    POPO_TRY(check_unique(txn, table, row));
    std::vector<char> old_bytes;
    if (auto old = visible_row(txn, table, pk, snapshot_csn_for(txn)))
    {
        old_bytes = popo::storage::RowSerializer::serialize(*old, heap->schema());
    }
    auto new_bytes = popo::storage::RowSerializer::serialize(row, heap->schema());
    log_operation(txn, LogType::UPDATE, table, old_bytes, new_bytes);
    publish_overlay(txn, table, pk, row);
    return {};
}

popo::base::Result<void> MvccManager::stage_delete(Transaction * txn, const std::string & table, const Value & pk)
{
    auto * heap = engine_->get_table_heap(table);
    if (heap == nullptr)
    {
        return popo::base::make_error(popo::base::Errno::TableNotFound, "MVCC: table not found: " + table);
    }
    std::vector<char> old_bytes;
    if (auto old = visible_row(txn, table, pk, snapshot_csn_for(txn)))
    {
        old_bytes = popo::storage::RowSerializer::serialize(*old, heap->schema());
    }
    log_operation(txn, LogType::DELETE, table, old_bytes, {});
    publish_overlay(txn, table, pk, std::nullopt);
    return {};
}

void MvccManager::publish_overlay(Transaction * txn, const std::string & table, const Value & pk, const std::optional<Row> & row)
{
    // Read-uncommitted readers walk other transactions' overlays under txn_mutex_,
    // so publishing a buffered write has to take it too.
    std::lock_guard<std::mutex> lock(txn_mutex_);
    txn->overlay()[table].rows[pk] = row;
}

popo::base::Result<void> MvccManager::check_unique(Transaction * txn, const std::string & table, const Row & row)
{
    auto * heap = engine_->get_table_heap(table);
    const auto & catalog = engine_->catalog();
    auto cat = catalog.find(table);
    if (heap == nullptr || cat == catalog.end() || cat->second.indexes_.empty())
    {
        return {};
    }
    const auto pk = heap->schema().extract_pk(row);
    auto own = txn->overlay().find(table);

    for (size_t i = 0; i < cat->second.indexes_.size(); ++i)
    {
        const auto & index = cat->second.indexes_[i];
        if (!index.is_unique_ || index.index_cols_.size() != 1 || index.index_cols_[0] < 0)
        {
            continue;
        }
        auto col = static_cast<size_t>(index.index_cols_[0]);
        if (col >= row.values_.size())
        {
            continue;
        }
        const auto & key = row.values_[col];

        // The index holds committed rows only, so buffered writes are candidates too.
        auto candidates = heap->lookup_index(i, key);
        if (own != txn->overlay().end())
        {
            for (const auto & [other_pk, other] : own->second.rows)
            {
                if (other && other->values_.size() > col && other->values_[col] == key)
                {
                    candidates.push_back(other_pk);
                }
            }
        }

        // What another primary key currently holds in this column, taking this
        // transaction's buffered deletes and updates into account.
        auto value_at = [&](const Value & other_pk) -> std::optional<Value>
        {
            if (own != txn->overlay().end())
            {
                if (auto r = own->second.rows.find(other_pk); r != own->second.rows.end())
                {
                    if (!r->second || r->second->values_.size() <= col)
                    {
                        return std::nullopt;
                    }
                    return r->second->values_[col];
                }
            }
            auto committed = lookup_committed(table, other_pk);
            if (!committed || committed->values_.size() <= col)
            {
                return std::nullopt;
            }
            return committed->values_[col];
        };

        for (const auto & other_pk : candidates)
        {
            if (other_pk == pk)
            {
                continue;
            }
            if (auto value = value_at(other_pk); value && *value == key)
            {
                return popo::base::make_error(
                    popo::base::Errno::DuplicateKey,
                    "Duplicate entry '" + key.to_val_string() + "' for key '" + index.index_name_ + "'");
            }
        }
    }
    return {};
}

bool MvccManager::primary_key_taken(Transaction * txn, const std::string & table, const Value & pk)
{
    // The heap holds current committed state and arbitrates primary keys, so a
    // row committed after this transaction began is still a duplicate; only this
    // transaction's own buffered delete frees the key.
    auto oit = txn->overlay().find(table);
    if (oit != txn->overlay().end())
    {
        auto rit = oit->second.rows.find(pk);
        if (rit != oit->second.rows.end())
        {
            return rit->second.has_value();
        }
    }
    return lookup_committed(table, pk).has_value();
}

std::optional<Row> MvccManager::visible_row(Transaction * txn, const std::string & table, const Value & pk, uint64_t snapshot_csn)
{
    // Own buffered writes always win.
    auto oit = txn->overlay().find(table);
    if (oit != txn->overlay().end())
    {
        auto rit = oit->second.rows.find(pk);
        if (rit != oit->second.rows.end())
        {
            return rit->second; // engaged row or disengaged (delete)
        }
    }

    // Read-uncommitted also observes other running transactions' overlays.
    if (txn->isolation() == IsolationLevel::ReadUncommitted)
    {
        std::lock_guard<std::mutex> lock(txn_mutex_);
        for (const auto & [id, other] : active_)
        {
            if (other.get() == txn)
            {
                continue;
            }
            auto ot = other->overlay().find(table);
            if (ot != other->overlay().end())
            {
                auto rt = ot->second.rows.find(pk);
                if (rt != ot->second.rows.end())
                {
                    return rt->second;
                }
            }
        }
    }

    std::shared_lock<std::shared_mutex> lock(version_mutex_);
    auto tit = versions_.find(table);
    if (tit != versions_.end())
    {
        auto pit = tit->second.find(pk);
        if (pit != tit->second.end())
        {
            for (const auto & v : pit->second)
            {
                if (visible(v, snapshot_csn))
                {
                    return v.row;
                }
            }
            // The key has history but no version existed at this snapshot.
            return std::nullopt;
        }
    }

    auto * heap = engine_->get_table_heap(table);
    if (heap == nullptr)
    {
        return std::nullopt;
    }
    auto rid = heap->lookup_pk(pk);
    if (!rid)
    {
        return std::nullopt;
    }
    auto tuple = heap->get_row(*rid);
    return tuple ? std::optional<Row>(tuple->deform(heap->schema())) : std::nullopt;
}

MvccManager::Cursor MvccManager::scan_cursor(
    Transaction * txn, const std::string & table, const std::optional<Value> & low, const std::optional<Value> & high)
{
    note_read(txn, table, low, high);
    return Cursor(std::make_unique<Cursor::Impl>(this, txn, table, low, high));
}

std::vector<Row> MvccManager::scan(Transaction * txn, const std::string & table)
{
    auto cursor = scan_cursor(txn, table);
    std::vector<Row> rows;
    Row row;
    while (cursor.next(row))
    {
        rows.push_back(std::move(row));
    }
    return rows;
}

int64_t MvccManager::count(Transaction * txn, const std::string & table)
{
    auto cursor = scan_cursor(txn, table);
    int64_t result = 0;
    Row row;
    while (cursor.next(row))
    {
        ++result;
    }
    return result;
}

std::optional<Row> MvccManager::lookup(Transaction * txn, const std::string & table, const Value & pk)
{
    note_read(txn, table, pk, pk);
    return visible_row(txn, table, pk, snapshot_csn_for(txn));
}

std::optional<Row> MvccManager::lookup_committed(const std::string & table, const Value & pk)
{
    // Serialize with install_overlay so a multi-row commit is observed wholly
    // before or wholly after publication.
    std::shared_lock<std::shared_mutex> lock(version_mutex_);
    auto * heap = engine_->get_table_heap(table);
    if (heap == nullptr)
    {
        return std::nullopt;
    }
    auto rid = heap->lookup_pk(pk);
    if (!rid)
    {
        return std::nullopt;
    }
    auto tuple = heap->get_row(*rid);
    return tuple ? std::optional<Row>(tuple->deform(heap->schema())) : std::nullopt;
}

void MvccManager::note_read(Transaction * txn, const std::string & table, const std::optional<Value> & low, const std::optional<Value> & high)
{
    if (txn->isolation() != IsolationLevel::Serializable)
    {
        return;
    }
    txn->read_set()[table].push_back(Transaction::ReadRange{low, high});
}

bool MvccManager::validate_serializable(Transaction * txn)
{
    // Conservative first-committer-wins: if anything this transaction read was
    // changed by a commit after its snapshot, abort. This prevents dirty reads,
    // non-repeatable reads, phantoms, and write skew.
    auto snapshot_csn = txn->snapshot().csn;
    auto now = current_csn();
    std::shared_lock<std::shared_mutex> lock(version_mutex_);
    ValueLess less;
    for (const auto & [table, ranges] : txn->read_set())
    {
        auto tit = versions_.find(table);
        if (tit == versions_.end())
        {
            continue;
        }
        for (const auto & range : ranges)
        {
            for (const auto & [pk, chain] : tit->second)
            {
                if (range.low && less(pk, *range.low))
                {
                    continue;
                }
                if (range.high && less(*range.high, pk))
                {
                    continue;
                }
                for (const auto & v : chain)
                {
                    bool created_after = v.begin_csn >= snapshot_csn && v.begin_csn < now;
                    bool deleted_after = v.end_csn != UINT64_MAX && v.end_csn >= snapshot_csn && v.end_csn < now;
                    if (created_after || deleted_after)
                    {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool MvccManager::has_write_conflict(Transaction * txn)
{
    auto snapshot_csn = txn->snapshot().csn;
    auto now = current_csn();
    std::shared_lock<std::shared_mutex> lock(version_mutex_);
    for (const auto & [table, overlay] : txn->overlay())
    {
        auto tit = versions_.find(table);
        if (tit == versions_.end())
        {
            continue;
        }
        for (const auto & [pk, opt] : overlay.rows)
        {
            auto pit = tit->second.find(pk);
            if (pit == tit->second.end())
            {
                continue;
            }
            for (const auto & v : pit->second)
            {
                bool created_after = v.begin_csn >= snapshot_csn && v.begin_csn < now;
                bool retired_after = v.end_csn != UINT64_MAX && v.end_csn >= snapshot_csn && v.end_csn < now;
                if (created_after || retired_after)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool MvccManager::install_row(const std::string & table, const Row & row)
{
    auto * heap = engine_->get_table_heap(table);
    if (heap == nullptr)
    {
        return false;
    }
    auto pk_res = primary_key_of(table, row);
    if (!pk_res)
    {
        return false;
    }
    const auto & pk = *pk_res;
    auto tuple = popo::storage::Tuple::form(heap->schema(), row);
    heap->set_log_wal(false);
    auto existing = heap->lookup_pk(pk);
    if (existing)
    {
        heap->delete_row(nullptr, *existing);
    }
    auto rid = heap->insert_row(nullptr, tuple);
    heap->set_log_wal(true);
    return rid.is_valid();
}

bool MvccManager::remove_row(const std::string & table, const Value & pk)
{
    auto * heap = engine_->get_table_heap(table);
    if (heap == nullptr)
    {
        return false;
    }
    heap->set_log_wal(false);
    auto existing = heap->lookup_pk(pk);
    bool ok = true;
    if (existing)
    {
        ok = heap->delete_row(nullptr, *existing);
    }
    heap->set_log_wal(true);
    return ok;
}

void MvccManager::install_overlay(Transaction * txn, uint64_t csn)
{
    std::unique_lock<std::shared_mutex> lock(version_mutex_);
    // The COMMIT record is already durable (flushed by commit()). Installing into
    // the in-memory heap and version store needs no further durability barrier:
    // recovery repeats this install from the WAL. Buffer-pool eviction persists
    // committed pages lazily; the heap root is rebuilt from the WAL on restart.
    for (const auto & [table, overlay] : txn->overlay())
    {
        auto & vtable = versions_[table];
        for (const auto & [pk, opt] : overlay.rows)
        {
            auto & chain = vtable[pk];
            if (chain.empty())
            {
                // Capture the old baseline only when this key is first touched.
                // Untouched committed rows stay exclusively in the B+Tree.
                auto * heap = engine_->get_table_heap(table);
                if (heap != nullptr)
                {
                    auto rid = heap->lookup_pk(pk);
                    if (rid)
                    {
                        auto tuple = heap->get_row(*rid);
                        if (tuple)
                        {
                            chain.push_back(Version{tuple->deform(heap->schema()), 0, csn});
                        }
                    }
                }
            }
            else if (chain.back().end_csn == UINT64_MAX)
            {
                chain.back().end_csn = csn;
            }
            chain.push_back(Version{opt, csn, UINT64_MAX});
            if (opt)
            {
                install_row(table, *opt);
            }
            else
            {
                remove_row(table, pk);
            }
        }
    }
}

popo::base::Result<void> MvccManager::commit(Transaction * txn)
{
    std::lock_guard<std::mutex> install_lock(commit_mutex_);

    if (txn->isolation() == IsolationLevel::Serializable && !validate_serializable(txn))
    {
        abort_locked(txn);
        return popo::base::make_error(popo::base::Errno::SerializationFailure, "serialization failure: transaction aborted");
    }

    if ((txn->isolation() == IsolationLevel::RepeatableRead || txn->isolation() == IsolationLevel::Serializable)
        && has_write_conflict(txn))
    {
        abort_locked(txn);
        return popo::base::make_error(
            popo::base::Errno::SerializationFailure, "serialization failure: concurrent write, transaction aborted");
    }

    auto csn = next_csn_.fetch_add(1);

    // Nothing was written, so there is nothing to make durable: recovery can
    // simply drop the BEGIN. This keeps autocommit reads off the fsync path.
    const bool read_only = txn->overlay().empty();
    if (!read_only)
    {
        // Durability point: COMMIT must be stable before any data is installed.
        LogRecord commit_rec;
        commit_rec.txn_id = txn->txn_id();
        commit_rec.type = LogType::COMMIT;
        commit_rec.prev_lsn = txn->last_lsn();
        auto commit_lsn = redo_log_->append(commit_rec);
        txn->set_last_lsn(commit_lsn);
        redo_log_->flush_to_lsn(commit_lsn);

        install_overlay(txn, csn);
    }

    {
        std::lock_guard<std::mutex> lock(txn_mutex_);
        status_[txn->txn_id()] = TxnStatus{TxnState::COMMITTED, csn};
        txn->set_state(TxnState::COMMITTED);
        txn->set_commit_csn(csn);
        active_.erase(txn->txn_id());
    }

    if (!read_only)
    {
        LogRecord end_rec;
        end_rec.txn_id = txn->txn_id();
        end_rec.type = LogType::END;
        end_rec.prev_lsn = txn->last_lsn();
        redo_log_->append(end_rec);
    }

    garbage_collect();
    return {};
}

void MvccManager::abort(Transaction * txn)
{
    std::lock_guard<std::mutex> install_lock(commit_mutex_);
    abort_locked(txn);
}

void MvccManager::abort_locked(Transaction * txn)
{
    // Compensation records for the undo chain (no shared pages were modified,
    // so the CLR carries the inverse intent for recovery repeat-history).
    const auto & writes = txn->write_set();
    for (auto it = writes.rbegin(); it != writes.rend(); ++it)
    {
        if (it->type != LogType::INSERT && it->type != LogType::DELETE && it->type != LogType::UPDATE)
        {
            continue;
        }
        LogRecord clr;
        clr.txn_id = txn->txn_id();
        clr.type = LogType::CLR;
        clr.table_name = it->table_name;
        clr.prev_lsn = txn->last_lsn();
        clr.undo_next_lsn = it->prev_lsn;
        clr.compensation = it->type == LogType::INSERT ? LogType::DELETE : LogType::INSERT;
        clr.old_data = it->new_data;
        clr.new_data = it->old_data;
        auto lsn = redo_log_->append(clr);
        txn->set_last_lsn(lsn);
    }

    LogRecord abort_rec;
    abort_rec.txn_id = txn->txn_id();
    abort_rec.type = LogType::ABORT;
    abort_rec.prev_lsn = txn->last_lsn();
    auto lsn = redo_log_->append(abort_rec);
    txn->set_last_lsn(lsn);
    redo_log_->flush_to_lsn(lsn);

    LogRecord end_rec;
    end_rec.txn_id = txn->txn_id();
    end_rec.type = LogType::END;
    end_rec.prev_lsn = txn->last_lsn();
    redo_log_->append(end_rec);

    std::lock_guard<std::mutex> lock(txn_mutex_);
    status_[txn->txn_id()] = TxnStatus{TxnState::ABORTED, 0};
    txn->set_state(TxnState::ABORTED);
    active_.erase(txn->txn_id());
}

void MvccManager::rebuild_from_heap(const std::string & table)
{
    // Recovery has already rebuilt the current committed B+Tree image. History
    // from before the crash has no surviving snapshots and need not be copied.
    std::unique_lock<std::shared_mutex> lock(version_mutex_);
    versions_.erase(table);
}

void MvccManager::forget_table(const std::string & table)
{
    std::unique_lock<std::shared_mutex> lock(version_mutex_);
    versions_.erase(table);
}

std::vector<Row> MvccManager::current_rows(const std::string & table)
{
    std::vector<Row> rows;
    auto * heap = engine_->get_table_heap(table);
    if (heap == nullptr)
    {
        return rows;
    }
    auto iter = heap->iter();
    for (; iter.valid(); iter.next())
    {
        rows.push_back(iter.current_row());
    }
    return rows;
}

void MvccManager::garbage_collect()
{
    uint64_t pin = current_csn();
    {
        std::lock_guard<std::mutex> lock(txn_mutex_);
        for (const auto & [id, txn] : active_)
        {
            if (txn->isolation() == IsolationLevel::RepeatableRead || txn->isolation() == IsolationLevel::Serializable)
            {
                pin = std::min(pin, txn->snapshot().csn);
            }
            else if (txn->isolation() == IsolationLevel::ReadCommitted && txn->statement_csn() != 0)
            {
                pin = std::min(pin, txn->statement_csn());
            }
        }
        for (auto it = status_.begin(); it != status_.end();)
        {
            if (it->second.csn != 0 && it->second.csn < pin)
            {
                it = status_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::unique_lock<std::shared_mutex> lock(version_mutex_);
    for (auto table_it = versions_.begin(); table_it != versions_.end();)
    {
        auto & vtable = table_it->second;
        for (auto key_it = vtable.begin(); key_it != vtable.end();)
        {
            auto & chain = key_it->second;
            // Keep the current version plus anything a pinned snapshot may see.
            chain.erase(
                std::remove_if(
                    chain.begin(), chain.end(),
                    [&](const Version & v) { return v.end_csn != UINT64_MAX && v.end_csn < pin; }),
                chain.end());

            // Once every snapshot is newer than the latest change, the B+Tree
            // (or its absence for a tombstone) is sufficient for this key.
            if (chain.empty() || (chain.size() == 1 && chain.back().begin_csn < pin))
            {
                key_it = vtable.erase(key_it);
            }
            else
            {
                ++key_it;
            }
        }
        if (vtable.empty())
        {
            table_it = versions_.erase(table_it);
        }
        else
        {
            ++table_it;
        }
    }
}

size_t MvccManager::retained_version_count() const
{
    std::shared_lock<std::shared_mutex> lock(version_mutex_);
    size_t count = 0;
    for (const auto & [table, vtable] : versions_)
    {
        for (const auto & [pk, chain] : vtable)
        {
            count += chain.size();
        }
    }
    return count;
}

} // namespace popo::txn