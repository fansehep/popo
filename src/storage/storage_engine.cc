#include "src/storage/storage_engine.h"
#include <filesystem>
#include <system_error>
#include "spdlog/spdlog.h"
#include "src/catalog/schema_io.h"
#include "src/storage/recovery.h"
#include "src/storage/row_serializer.h"

namespace popo::storage
{

StorageEngine::StorageEngine(const std::string & data_dir, size_t buffer_pool_size)
    : data_dir_(data_dir), buffer_pool_size_(buffer_pool_size)
{
}

StorageEngine::~StorageEngine()
{
    shutdown();
}

popo::base::Result<void> StorageEngine::init()
{
    std::error_code ec;
    std::filesystem::create_directories(data_dir_, ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "failed to create data_dir " + data_dir_ + ": " + ec.message());
    }

    disk_manager_ = std::make_unique<DiskManager>(data_dir_);
    POPO_TRY(disk_manager_->status());
    buffer_pool_ = std::make_unique<BufferPoolManager>(buffer_pool_size_, disk_manager_.get());
    redo_log_ = std::make_unique<RedoLogManager>(data_dir_ + "/redo.log");
    txn_mgr_ = std::make_unique<popo::txn::TxnManager>(redo_log_.get());
    mvcc_ = std::make_unique<popo::txn::MvccManager>(this, redo_log_.get());

    POPO_TRY(recover());

    spdlog::info("StorageEngine initialized with data_dir={}", data_dir_);
    return {};
}

void StorageEngine::shutdown()
{
    if (mvcc_ && disk_manager_)
    {
        // Publish a final checkpoint so the next start replays an empty WAL.
        checkpoint();
    }
    if (redo_log_)
    {
        redo_log_->flush();
    }
    table_heaps_.clear();
    catalog_.clear();
    mvcc_.reset();
    txn_mgr_.reset();
    redo_log_.reset();
    buffer_pool_.reset();
    disk_manager_.reset();
}

popo::base::Result<void> StorageEngine::recover()
{
    RecoveryManager recovery(this, redo_log_.get());
    POPO_TRY(recovery.recover());
    spdlog::info("StorageEngine::recover: ARIES replay complete for {}", data_dir_);
    return {};
}

TableHeap * StorageEngine::get_table_heap(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_heaps_.find(table_name);
    if (it == table_heaps_.end())
    {
        return nullptr;
    }
    return it->second.get();
}

void StorageEngine::log_ddl(LogType type, const std::string & table, const popo::catalog::Schema * schema)
{
    LogRecord rec;
    rec.type = type;
    rec.table_name = table;
    rec.txn_id = INVALID_TXN_ID;
    if (schema != nullptr)
    {
        rec.new_data = popo::catalog::serialize_schema(*schema);
    }
    auto lsn = redo_log_->append(rec);
    redo_log_->flush_to_lsn(lsn);
}

popo::base::Result<void> StorageEngine::create_table(const std::string & table_name, const popo::catalog::Schema & schema)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (table_heaps_.find(table_name) != table_heaps_.end())
    {
        spdlog::warn("StorageEngine::create_table: table {} already exists", table_name);
        return {};
    }

    POPO_TRY(disk_manager_->create_table_file(table_name));
    auto heap = std::make_unique<TableHeap>(table_name, buffer_pool_.get(), redo_log_.get(), schema);
    table_heaps_[table_name] = std::move(heap);
    catalog_[table_name] = schema;

    log_ddl(LogType::CREATE_TABLE, table_name, &schema);
    spdlog::info("StorageEngine::create_table: created table {}", table_name);
    return {};
}

popo::base::Result<void> StorageEngine::create_index(
    const std::string & table_name, const std::string & index_name, int32_t column, bool unique)
{
    std::lock_guard<std::mutex> lock(mutex_);
    POPO_TRY(attach_secondary_index(table_name, index_name, column, unique));
    auto & cat = catalog_[table_name];
    cat.indexes_.push_back(popo::catalog::SchemaIndex{index_name, {column}, unique});
    log_ddl(LogType::CREATE_INDEX, table_name, &cat);
    return {};
}

popo::base::Result<void> StorageEngine::attach_secondary_index(
    const std::string & table_name, const std::string & index_name, int32_t column, bool unique)
{
    auto it = table_heaps_.find(table_name);
    if (it == table_heaps_.end())
    {
        return popo::base::make_error(popo::base::Errno::TableNotFound, "create index: table not found");
    }
    const auto & base = it->second->schema();
    if (base.columns_.empty() || column < 0 || column >= static_cast<int32_t>(base.columns_.size()))
    {
        return popo::base::make_error(popo::base::Errno::InvalidArg, "create index: bad column");
    }
    popo::catalog::Schema index_schema;
    index_schema.table_name_ = base.table_name_;
    popo::catalog::Column key = base.columns_[static_cast<size_t>(column)];
    key.column_name_ = "k";
    key.is_primary_key_ = true;
    popo::catalog::Column pk;
    if (base.primary_key_col_idx_ >= 0)
    {
        pk = base.columns_[static_cast<size_t>(base.primary_key_col_idx_)];
    }
    else
    {
        pk.data_type_ = popo::type::ValueType::String;
    }
    pk.column_name_ = "pk";
    pk.is_primary_key_ = false;
    index_schema.columns_ = {key, pk};
    index_schema.primary_key_col_idx_ = 0;
    auto tree = std::make_unique<TableHeap>(base.table_name_, buffer_pool_.get(), redo_log_.get(), index_schema);
    tree->set_sort_width(2);
    it->second->attach_index(index_name, column, unique, std::move(tree));
    return {};
}

void StorageEngine::drop_table(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (mvcc_)
    {
        mvcc_->forget_table(table_name);
    }
    table_heaps_.erase(table_name);
    catalog_.erase(table_name);
    // The file is about to disappear, so its dirty frames must never be flushed.
    buffer_pool_->invalidate_table(table_name);
    disk_manager_->remove_table_file(table_name);
    log_ddl(LogType::DROP_TABLE, table_name, nullptr);
    spdlog::info("StorageEngine::drop_table: dropped table {}", table_name);
}

bool StorageEngine::table_exists(const std::string & table_name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return table_heaps_.find(table_name) != table_heaps_.end();
}

// ---- Recovery hooks (called by RecoveryManager; no DDL re-logging) ----

popo::base::Result<void> StorageEngine::recover_create_table(const std::string & table_name, const popo::catalog::Schema & schema)
{
    if (table_heaps_.find(table_name) != table_heaps_.end())
    {
        return {};
    }
    // The WAL is the recovery authority; start from a fresh table file so stale
    // page generations from a previous run cannot collide with rebuilt roots.
    disk_manager_->remove_table_file(table_name);
    POPO_TRY(disk_manager_->create_table_file(table_name));
    auto heap = std::make_unique<TableHeap>(table_name, buffer_pool_.get(), redo_log_.get(), schema);
    table_heaps_[table_name] = std::move(heap);
    catalog_[table_name] = schema;
    for (const auto & idx : schema.indexes_)
    {
        if (!idx.index_cols_.empty())
        {
            POPO_TRY(attach_secondary_index(table_name, idx.index_name_, idx.index_cols_[0], idx.is_unique_));
        }
    }
    return {};
}

popo::base::Result<void> StorageEngine::recover_create_index(
    const std::string & table_name, const std::string & index_name, int32_t column, bool unique)
{
    if (table_heaps_.find(table_name) == table_heaps_.end())
    {
        return {};
    }
    auto & cat = catalog_[table_name];
    for (const auto & idx : cat.indexes_)
    {
        if (idx.index_name_ == index_name)
        {
            return {}; // already attached
        }
    }
    POPO_TRY(attach_secondary_index(table_name, index_name, column, unique));
    cat.indexes_.push_back(popo::catalog::SchemaIndex{index_name, {column}, unique});
    return {};
}

void StorageEngine::recover_drop_table(const std::string & table_name)
{
    if (mvcc_)
    {
        mvcc_->forget_table(table_name);
    }
    table_heaps_.erase(table_name);
    catalog_.erase(table_name);
    buffer_pool_->invalidate_table(table_name);
    disk_manager_->remove_table_file(table_name);
}

popo::txn::Transaction * StorageEngine::begin_txn()
{
    return mvcc_->begin(mvcc_->default_isolation());
}

popo::txn::Transaction * StorageEngine::begin_txn(popo::txn::IsolationLevel level)
{
    return mvcc_->begin(level);
}

popo::base::Result<void> StorageEngine::commit_txn(popo::txn::Transaction * txn)
{
    return mvcc_->commit(txn);
}

void StorageEngine::abort_txn(popo::txn::Transaction * txn)
{
    mvcc_->abort(txn);
}

void StorageEngine::checkpoint()
{
    std::vector<LogRecord> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        LogRecord begin;
        begin.type = LogType::BEGIN;
        begin.txn_id = 0;
        snapshot.push_back(begin);
        for (const auto & [name, schema] : catalog_)
        {
            auto hit = table_heaps_.find(name);
            if (hit == table_heaps_.end() || !hit->second)
            {
                continue;
            }
            LogRecord ct;
            ct.type = LogType::CREATE_TABLE;
            ct.table_name = name;
            ct.txn_id = 0;
            ct.new_data = popo::catalog::serialize_schema(schema);
            snapshot.push_back(std::move(ct));
            auto * heap = hit->second.get();
            for (const auto & row : mvcc_->current_rows(name))
            {
                LogRecord ins;
                ins.type = LogType::INSERT;
                ins.table_name = name;
                ins.txn_id = 0;
                ins.new_data = RowSerializer::serialize(row, heap->schema());
                snapshot.push_back(std::move(ins));
            }
        }
        LogRecord commit;
        commit.type = LogType::COMMIT;
        commit.txn_id = 0;
        snapshot.push_back(commit);
    }
    redo_log_->truncate();
    for (const auto & rec : snapshot)
    {
        redo_log_->append(rec);
    }
    redo_log_->flush();
}

popo::base::Result<void> StorageEngine::flush_all()
{
    if (buffer_pool_)
    {
        return buffer_pool_->flush_all();
    }
    return {};
}

} // namespace popo::storage
