#pragma once
#include "src/storage/common.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/redo_log.h"
#include "src/storage/table_heap.h"
#include "src/trancation/manager.h"
#include "src/catalog/schema.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

namespace neon::storage {

class StorageEngine {
public:
    // Initialize with data directory path
    explicit StorageEngine(const std::string& data_dir, size_t buffer_pool_size = 1024);
    ~StorageEngine();

    // Lifecycle
    void init();       // Initialize components, run recovery
    void shutdown();   // Flush all, close files
    void recover();    // Replay redo log on startup

    // Table management
    TableHeap* get_table_heap(const std::string& table_name);
    void create_table(const std::string& table_name, const neon::catalog::Schema& schema);
    void drop_table(const std::string& table_name);
    bool table_exists(const std::string& table_name) const;

    // Transaction interface (delegates to TxnManager)
    neon::txn::Transaction* begin_txn();
    void commit_txn(neon::txn::Transaction* txn);
    void abort_txn(neon::txn::Transaction* txn);

    // Component access
    BufferPoolManager* buffer_pool() { return buffer_pool_.get(); }
    DiskManager* disk_manager() { return disk_manager_.get(); }
    RedoLogManager* redo_log() { return redo_log_.get(); }
    neon::txn::TxnManager* txn_manager() { return txn_mgr_.get(); }

    // Flush all dirty pages
    void flush_all();

private:
    std::string data_dir_;
    size_t buffer_pool_size_;

    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_;
    std::unique_ptr<RedoLogManager> redo_log_;
    std::unique_ptr<neon::txn::TxnManager> txn_mgr_;

    std::unordered_map<std::string, std::unique_ptr<TableHeap>> table_heaps_;
    mutable std::mutex mutex_;
};

}  // namespace neon::storage
