#include "src/storage/storage_engine.h"
#include <filesystem>
#include <stdexcept>
#include "spdlog/spdlog.h"

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

void StorageEngine::init()
{
    // Ensure data directory exists
    std::filesystem::create_directories(data_dir_);

    // Create components
    disk_manager_ = std::make_unique<DiskManager>(data_dir_);
    buffer_pool_ = std::make_unique<BufferPoolManager>(buffer_pool_size_, disk_manager_.get());
    redo_log_ = std::make_unique<RedoLogManager>(data_dir_ + "/redo.log");
    txn_mgr_ = std::make_unique<popo::txn::TxnManager>(redo_log_.get());

    // Run recovery if there are log records
    recover();

    spdlog::info("StorageEngine initialized with data_dir={}", data_dir_);
}

void StorageEngine::shutdown()
{
    if (disk_manager_ && disk_manager_->in_txn())
    {
        disk_manager_->abort_txn();
        if (buffer_pool_)
        {
            buffer_pool_->invalidate_all();
        }
        for (const auto & [name, snap] : root_snap_)
        {
            if (auto it = table_heaps_.find(name); it != table_heaps_.end())
            {
                it->second->set_root_page_id(snap.root);
                it->second->set_index_roots(snap.index_roots);
            }
        }
        root_snap_.clear();
    }
    else if (buffer_pool_)
    {
        flush_all();
    }
    if (redo_log_)
    {
        redo_log_->flush();
    }
    table_heaps_.clear();
    txn_mgr_.reset();
    redo_log_.reset();
    buffer_pool_.reset();
    disk_manager_.reset();
}

void StorageEngine::recover()
{
    // Committed page images are already visible from the block headers.
    // Uncommitted generations are ignored. There is no row undo/redo replay.
    spdlog::info("StorageEngine::recover: using committed page generations in {}", data_dir_);
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

void StorageEngine::create_table(const std::string & table_name, const popo::catalog::Schema & schema)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (table_heaps_.find(table_name) != table_heaps_.end())
    {
        spdlog::warn("StorageEngine::create_table: table {} already exists", table_name);
        return;
    }

    disk_manager_->create_table_file(table_name).value();

    auto heap = std::make_unique<TableHeap>(table_name, buffer_pool_.get(), redo_log_.get(), schema);
    table_heaps_[table_name] = std::move(heap);

    spdlog::info("StorageEngine::create_table: created table {}", table_name);
}

void StorageEngine::create_index(const std::string & table_name, const std::string & index_name, int32_t column, bool unique)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_heaps_.find(table_name);
    if (it == table_heaps_.end())
    {
        throw std::runtime_error("create index: table not found");
    }
    const auto & base = it->second->schema();
    if (base.primary_key_col_idx_ < 0 || column < 0 || column >= static_cast<int32_t>(base.columns_.size()))
    {
        throw std::runtime_error("create index: bad column");
    }
    popo::catalog::Schema index_schema;
    index_schema.table_name_ = base.table_name_;
    popo::catalog::Column key = base.columns_[static_cast<size_t>(column)];
    key.column_name_ = "k";
    key.is_primary_key_ = true;
    popo::catalog::Column pk = base.columns_[static_cast<size_t>(base.primary_key_col_idx_)];
    pk.column_name_ = "pk";
    pk.is_primary_key_ = false;
    index_schema.columns_ = {key, pk};
    index_schema.primary_key_col_idx_ = 0;
    auto tree = std::make_unique<TableHeap>(base.table_name_, buffer_pool_.get(), redo_log_.get(), index_schema);
    tree->set_sort_width(2);
    it->second->attach_index(index_name, column, unique, std::move(tree));
}

void StorageEngine::drop_table(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);

    table_heaps_.erase(table_name);
    disk_manager_->remove_table_file(table_name);

    spdlog::info("StorageEngine::drop_table: dropped table {}", table_name);
}

bool StorageEngine::table_exists(const std::string & table_name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return table_heaps_.find(table_name) != table_heaps_.end();
}

popo::txn::Transaction * StorageEngine::begin_txn()
{
    if (!disk_manager_->begin_txn())
    {
        throw std::runtime_error("only one transaction is supported");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root_snap_.clear();
        for (const auto & [name, heap] : table_heaps_)
        {
            RootSnap snap;
            snap.root = heap->get_root_page_id();
            snap.index_roots = heap->index_roots();
            root_snap_[name] = std::move(snap);
        }
    }
    try
    {
        return txn_mgr_->begin_txn();
    }
    catch (...)
    {
        disk_manager_->abort_txn();
        std::lock_guard<std::mutex> lock(mutex_);
        root_snap_.clear();
        throw;
    }
}

void StorageEngine::commit_txn(popo::txn::Transaction * txn)
{
    flush_all();
    disk_manager_->commit_txn();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root_snap_.clear();
    }
    txn_mgr_->commit(txn);
}

void StorageEngine::abort_txn(popo::txn::Transaction * txn)
{
    if (disk_manager_)
    {
        disk_manager_->abort_txn();
    }
    if (buffer_pool_)
    {
        buffer_pool_->invalidate_all();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto & [name, snap] : root_snap_)
        {
            if (auto it = table_heaps_.find(name); it != table_heaps_.end())
            {
                it->second->set_root_page_id(snap.root);
                it->second->set_index_roots(snap.index_roots);
                it->second->clear_ahi();
            }
        }
        root_snap_.clear();
    }
    txn_mgr_->abort(txn);
}

void StorageEngine::flush_all()
{
    if (buffer_pool_)
    {
        buffer_pool_->flush_all();
    }
}

} // namespace popo::storage
