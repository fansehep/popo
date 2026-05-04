#include "src/storage/storage_engine.h"
#include "src/storage/page.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <set>

namespace neon::storage {

StorageEngine::StorageEngine(const std::string& data_dir, size_t buffer_pool_size)
    : data_dir_(data_dir), buffer_pool_size_(buffer_pool_size) {}

StorageEngine::~StorageEngine() {
    shutdown();
}

void StorageEngine::init() {
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

void StorageEngine::shutdown() {
    if (buffer_pool_) {
        flush_all();
    }
    if (redo_log_) {
        redo_log_->flush();
    }
    table_heaps_.clear();
    txn_mgr_.reset();
    redo_log_.reset();
    buffer_pool_.reset();
    disk_manager_.reset();
}

void StorageEngine::recover() {
    if (!redo_log_) {
        return;
    }

    auto records = redo_log_->read_all_records();
    if (records.empty()) {
        return;
    }

    spdlog::info("StorageEngine::recover: replaying {} log records", records.size());

    // First pass: identify committed transactions
    std::set<txn_id_t> committed_txns;
    std::set<txn_id_t> aborted_txns;
    for (const auto& rec : records) {
        if (rec.type == LogType::COMMIT) {
            committed_txns.insert(rec.txn_id);
        } else if (rec.type == LogType::ABORT) {
            aborted_txns.insert(rec.txn_id);
        }
    }

    // Second pass: redo committed transactions
    for (const auto& rec : records) {
        if (committed_txns.find(rec.txn_id) == committed_txns.end()) {
            continue;  // Skip uncommitted txn records
        }

        if (rec.type == LogType::INSERT) {
            // Ensure table file exists
            if (!disk_manager_->table_file_exists(rec.table_name)) {
                continue;
            }
            auto guard = buffer_pool_->fetch_page(rec.table_name, rec.page_id);
            if (!guard.is_valid()) {
                continue;
            }
            auto* page = reinterpret_cast<DataPage*>(guard.data());
            page->insert_tuple(rec.new_data.data(), static_cast<uint16_t>(rec.new_data.size()));
            page->header()->lsn = rec.lsn;
            guard.mark_dirty();

        } else if (rec.type == LogType::DELETE) {
            if (!disk_manager_->table_file_exists(rec.table_name)) {
                continue;
            }
            auto guard = buffer_pool_->fetch_page(rec.table_name, rec.page_id);
            if (!guard.is_valid()) {
                continue;
            }
            auto* page = reinterpret_cast<DataPage*>(guard.data());
            page->delete_tuple(rec.slot_id);
            page->header()->lsn = rec.lsn;
            guard.mark_dirty();

        } else if (rec.type == LogType::UPDATE) {
            if (!disk_manager_->table_file_exists(rec.table_name)) {
                continue;
            }
            auto guard = buffer_pool_->fetch_page(rec.table_name, rec.page_id);
            if (!guard.is_valid()) {
                continue;
            }
            auto* page = reinterpret_cast<DataPage*>(guard.data());
            // Delete old, insert new
            page->delete_tuple(rec.slot_id);
            page->insert_tuple(rec.new_data.data(), static_cast<uint16_t>(rec.new_data.size()));
            page->header()->lsn = rec.lsn;
            guard.mark_dirty();
        }
    }

    // Flush all recovered pages and truncate log
    flush_all();
    redo_log_->truncate();

    spdlog::info("StorageEngine::recover: recovery complete");
}

TableHeap* StorageEngine::get_table_heap(const std::string& table_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_heaps_.find(table_name);
    if (it == table_heaps_.end()) {
        return nullptr;
    }
    return it->second.get();
}

void StorageEngine::create_table(const std::string& table_name, const neon::catalog::Schema& schema) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (table_heaps_.find(table_name) != table_heaps_.end()) {
        spdlog::warn("StorageEngine::create_table: table {} already exists", table_name);
        return;
    }

    disk_manager_->create_table_file(table_name);

    auto heap = std::make_unique<TableHeap>(table_name, buffer_pool_.get(), redo_log_.get(), schema);
    table_heaps_[table_name] = std::move(heap);

    spdlog::info("StorageEngine::create_table: created table {}", table_name);
}

void StorageEngine::drop_table(const std::string& table_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    table_heaps_.erase(table_name);
    disk_manager_->remove_table_file(table_name);

    spdlog::info("StorageEngine::drop_table: dropped table {}", table_name);
}

bool StorageEngine::table_exists(const std::string& table_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_heaps_.find(table_name) != table_heaps_.end();
}

neon::txn::Transaction* StorageEngine::begin_txn() {
    return txn_mgr_->begin_txn();
}

void StorageEngine::commit_txn(neon::txn::Transaction* txn) {
    txn_mgr_->commit(txn);
}

void StorageEngine::abort_txn(neon::txn::Transaction* txn) {
    txn_mgr_->abort(txn);
}

void StorageEngine::flush_all() {
    if (buffer_pool_) {
        buffer_pool_->flush_all();
    }
}

}  // namespace neon::storage
