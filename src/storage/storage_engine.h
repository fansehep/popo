#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "src/base/error.h"
#include "src/catalog/schema.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/common.h"
#include "src/storage/disk_manager.h"
#include "src/storage/redo_log.h"
#include "src/storage/table_heap.h"
#include "src/trancation/manager.h"
#include "src/trancation/mvcc.h"

namespace popo::storage
{

class StorageEngine
{
public:
    explicit StorageEngine(const std::string & data_dir, size_t buffer_pool_size = 1024);
    ~StorageEngine();

    // Lifecycle
    popo::base::Result<void> init();
    void shutdown();
    popo::base::Result<void> recover();

    // Table management
    TableHeap * get_table_heap(const std::string & table_name);
    popo::base::Result<void> create_table(const std::string & table_name, const popo::catalog::Schema & schema);
    popo::base::Result<void> create_index(const std::string & table_name, const std::string & index_name, int32_t column, bool unique);
    void drop_table(const std::string & table_name);
    bool table_exists(const std::string & table_name) const;

    const std::unordered_map<std::string, popo::catalog::Schema> & catalog() const { return catalog_; }

    // Transaction interface (MVCC + ARIES).
    popo::txn::Transaction * begin_txn();
    popo::txn::Transaction * begin_txn(popo::txn::IsolationLevel level);
    popo::base::Result<void> commit_txn(popo::txn::Transaction * txn);
    void abort_txn(popo::txn::Transaction * txn);

    // Component access
    BufferPoolManager * buffer_pool() { return buffer_pool_.get(); }
    DiskManager * disk_manager() { return disk_manager_.get(); }
    RedoLogManager * redo_log() { return redo_log_.get(); }
    popo::txn::TxnManager * txn_manager() { return txn_mgr_.get(); }
    popo::txn::MvccManager * mvcc() { return mvcc_.get(); }

    void set_default_isolation(popo::txn::IsolationLevel level) { mvcc_->set_default_isolation(level); }
    popo::txn::IsolationLevel default_isolation() const { return mvcc_->default_isolation(); }

    void checkpoint();

    popo::base::Result<void> flush_all();

    // Recovery hooks used by RecoveryManager (no DDL re-logging).
    popo::base::Result<void> recover_create_table(const std::string & table_name, const popo::catalog::Schema & schema);
    popo::base::Result<void> recover_create_index(const std::string & table_name, const std::string & index_name, int32_t column, bool unique);
    void recover_drop_table(const std::string & table_name);

private:
    std::string data_dir_;
    size_t buffer_pool_size_;
    struct RootSnap
    {
        page_id_t root = INVALID_PAGE_ID;
        std::vector<page_id_t> index_roots;
    };
    std::unordered_map<std::string, RootSnap> root_snap_;

    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_;
    std::unique_ptr<RedoLogManager> redo_log_;
    std::unique_ptr<popo::txn::TxnManager> txn_mgr_;
    std::unique_ptr<popo::txn::MvccManager> mvcc_;

    std::unordered_map<std::string, std::unique_ptr<TableHeap>> table_heaps_;
    mutable std::mutex mutex_;

    std::unordered_map<std::string, popo::catalog::Schema> catalog_;
    void log_ddl(popo::storage::LogType type, const std::string & table, const popo::catalog::Schema * schema);
    popo::base::Result<void> attach_secondary_index(const std::string & table, const std::string & name, int32_t column, bool unique);
};

} // namespace popo::storage
