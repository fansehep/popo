#include "src/storage/storage_engine.h"
#include <filesystem>
#include <stdexcept>
#include "spdlog/spdlog.h"

namespace neon::storage
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
    txn_mgr_ = std::make_unique<neon::txn::TxnManager>(redo_log_.get());

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
        for (const auto & [name, root] : root_snap_)
        {
            if (auto it = table_heaps_.find(name); it != table_heaps_.end())
            {
                it->second->set_root_page_id(root);
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

void StorageEngine::create_table(const std::string & table_name, const neon::catalog::Schema & schema)
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

neon::txn::Transaction * StorageEngine::begin_txn()
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
            root_snap_[name] = heap->get_root_page_id();
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

void StorageEngine::commit_txn(neon::txn::Transaction * txn)
{
    flush_all();
    disk_manager_->commit_txn();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root_snap_.clear();
    }
    txn_mgr_->commit(txn);
}

void StorageEngine::abort_txn(neon::txn::Transaction * txn)
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
        for (const auto & [name, root] : root_snap_)
        {
            if (auto it = table_heaps_.find(name); it != table_heaps_.end())
            {
                it->second->set_root_page_id(root);
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

} // namespace neon::storage
