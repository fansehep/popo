#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "src/base/error.h"
#include "src/storage/block.h"
#include "src/storage/common.h"

namespace neon::storage
{

class IoRing;

// One data file per table. Logical pages stay PAGE_SIZE bytes. Physically they
// are zstd-compressed and appended into 1MB blocks. Commit appends one record
// to commit.wal and fsyncs that file once, so a transaction that dirties
// several tables still has a single durability barrier. Abort throws that
// generation away.
class DiskManager
{
public:
    explicit DiskManager(const std::string & data_dir);
    ~DiskManager();

    DiskManager(const DiskManager &) = delete;
    DiskManager & operator=(const DiskManager &) = delete;

    neon::base::Result<void> create_table_file(const std::string & table_name);
    void remove_table_file(const std::string & table_name);

    neon::base::Result<void> read_page(const std::string & table_name, page_id_t page_id, char * dest);
    neon::base::Result<void> write_page(const std::string & table_name, page_id_t page_id, const char * src);
    neon::base::Result<page_id_t> allocate_page(const std::string & table_name);
    page_id_t page_count(const std::string & table_name);
    bool table_file_exists(const std::string & table_name) const;

    // Single-writer transaction. begin_txn returns true when this call opened it.
    bool begin_txn();
    void commit_txn();
    void abort_txn();
    bool in_txn() const;

    // Drop a logical page. The tombstone is published with the current transaction.
    void retire_page(const std::string & table_name, page_id_t page_id);

    const std::string & data_dir() const { return data_dir_; }

private:
    struct PageLoc
    {
        uint32_t block_id = NO_BLOCK;
        uint32_t offset = 0;
        uint32_t comp_size = 0;
        uint64_t gen = 0;
        bool tombstone = false;
    };

    struct TableSnap
    {
        uint32_t block_count = 0;
        uint32_t next_page_id = 1;
        uint32_t open_block = NO_BLOCK;
        std::vector<uint32_t> free_blocks;
        std::vector<BlockMeta> blocks;
        std::vector<uint8_t> block_dirty;
    };

    struct PendingWrite
    {
        uint64_t offset = 0;
        page_id_t page_id = 0;
        uint64_t gen = 0;
        // Shared with the page cache so a transaction that rewrites a page
        // keeps every version for the commit record without a second buffer.
        std::shared_ptr<std::vector<char>> bytes;
    };

    struct TableState
    {
        int fd = -1;
        FileHeader header{};
        alignas(8) char header_buf[FILE_HEADER_SIZE]{};
        std::vector<BlockMeta> blocks;
        std::vector<uint8_t> block_dirty;
        std::vector<uint32_t> free_blocks;
        std::unordered_map<page_id_t, PageLoc> committed;
        std::unordered_map<page_id_t, PageLoc> overlay;
        std::deque<PendingWrite> pending_io;
        std::unordered_map<page_id_t, std::shared_ptr<std::vector<char>>> pending_pages;
        std::unordered_map<page_id_t, std::shared_ptr<std::vector<char>>> cached_pages;
        bool participating = false;
        bool grew = false;
        uint64_t txn_gen = 0;
        TableSnap snap{};
    };


    std::string data_dir_;
    mutable std::mutex mutex_;
    std::unique_ptr<IoRing> io_;
    void * cctx_ = nullptr;
    void * dctx_ = nullptr;
    std::vector<char> comp_buf_;
    std::unordered_map<std::string, TableState> tables_;
    int wal_fd_ = -1;
    uint64_t wal_end_ = FILE_HEADER_SIZE;
    uint64_t wal_alloc_ = FILE_HEADER_SIZE;
    std::vector<char> wal_payload_;
    std::vector<char> wal_record_;
    bool in_txn_ = false;
    bool in_gc_ = false;

    std::string table_file_path(const std::string & table_name) const;
    TableState * find_table(const std::string & table_name);
    const TableState * find_table(const std::string & table_name) const;

    neon::base::Result<void> load_table(const std::string & table_name, int fd);
    neon::base::Result<void> write_header(TableState & table);
    neon::base::Result<void> write_block_meta(TableState & table, uint32_t block_id);
    neon::base::Result<void> read_block_meta(TableState & table, uint32_t block_id, BlockMeta & meta);

    void touch(TableState & table);
    const PageLoc * lookup(const TableState & table, page_id_t page_id) const;
    neon::base::Result<uint32_t> alloc_block(TableState & table);
    neon::base::Result<void> append_entry(TableState & table, page_id_t page_id, const char * comp, uint32_t comp_size, bool tombstone);
    neon::base::Result<void> append_wal_commit();
    neon::base::Result<void> durable_wal_write(const char * data, uint32_t len);
    void publish_memory(TableState & table);
    void open_wal();
    void replay_wal();
    void reset_wal();
    neon::base::Result<uint32_t> compress_page(const char * src);
    neon::base::Result<void> decompress_page(const char * src, size_t src_len, char * dest);
    void abort_table(TableState & table);
    void commit_unlocked();
    void abort_unlocked();
    uint64_t dead_bytes(const TableState & table) const;
    void maybe_gc(TableState & table);
    neon::base::Result<void> gc_table(TableState & table);
};

} // namespace neon::storage
