#include "src/storage/disk_manager.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <zstd.h>
#include "src/storage/io_ring.h"

namespace neon::storage
{

namespace
{

constexpr uint32_t WAL_MAGIC = 0x4C41574Eu; // 'NWAL'
constexpr uint32_t WAL_VERSION = 1;
constexpr uint32_t WAL_REC_MAGIC = 0x4345524Eu; // 'NREC'
constexpr uint64_t WAL_CHUNK = 1ull << 20;
constexpr uint32_t WAL_REC_LIMIT = 256u << 20;

uint32_t wal_crc(const char * data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= static_cast<uint8_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit)
        {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void append_raw(std::vector<char> & out, const void * data, size_t len)
{
    const char * bytes = static_cast<const char *>(data);
    out.insert(out.end(), bytes, bytes + len);
}

template <typename T>
void append_pod(std::vector<char> & out, const T & value)
{
    append_raw(out, &value, sizeof(T));
}

struct WalCursor
{
    const char * data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool ok = true;

    template <typename T>
    T take()
    {
        if (!ok || pos + sizeof(T) > size)
        {
            ok = false;
            return T{};
        }
        T value{};
        std::memcpy(&value, data + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }

    std::string take_name(uint32_t len)
    {
        if (!ok || pos + len > size)
        {
            ok = false;
            return {};
        }
        std::string name(data + pos, data + pos + len);
        pos += len;
        return name;
    }
};

bool block_has_space(const BlockMeta & meta, uint32_t comp_size)
{
    if (meta.flags == BLOCK_FREE || meta.flags == BLOCK_SEALED)
    {
        return false;
    }
    if (meta.page_count >= MAX_PAGES_PER_BLOCK)
    {
        return false;
    }
    return meta.payload_end + comp_size <= BLOCK_SIZE;
}

} // namespace

DiskManager::DiskManager(const std::string & data_dir) : data_dir_(data_dir), io_(std::make_unique<IoRing>())
{
    cctx_ = ZSTD_createCCtx();
    dctx_ = ZSTD_createDCtx();
    if (cctx_ == nullptr || dctx_ == nullptr)
    {
        ZSTD_freeCCtx(static_cast<ZSTD_CCtx *>(cctx_));
        ZSTD_freeDCtx(static_cast<ZSTD_DCtx *>(dctx_));
        cctx_ = nullptr;
        dctx_ = nullptr;
        throw std::runtime_error("failed to create zstd context");
    }
    // Negative levels are zstd's fast strategy. Page images are sparse, and the
    // commit barrier waits on the log, so encode time matters more than ratio.
    int level = 1;
    if (level < ZSTD_minCLevel())
    {
        level = ZSTD_minCLevel();
    }
    ZSTD_CCtx_setParameter(static_cast<ZSTD_CCtx *>(cctx_), ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(static_cast<ZSTD_CCtx *>(cctx_), ZSTD_c_checksumFlag, 0);
    ZSTD_CCtx_setParameter(static_cast<ZSTD_CCtx *>(cctx_), ZSTD_c_contentSizeFlag, 0);
    comp_buf_.resize(ZSTD_compressBound(PAGE_SIZE));
    std::filesystem::create_directories(data_dir_);
    if (!std::filesystem::exists(data_dir_))
    {
        return;
    }
    open_wal();
    for (const auto & entry : std::filesystem::directory_iterator(data_dir_))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".ndb")
        {
            continue;
        }
        std::string table_name = entry.path().stem().string();
        int fd = ::open(entry.path().c_str(), O_RDWR);
        if (fd < 0)
        {
            throw std::runtime_error("Failed to open table file: " + entry.path().string());
        }
        auto loaded = load_table(table_name, fd);
        if (!loaded)
        {
            ::close(fd);
            throw std::runtime_error(loaded.error().error_message);
        }
    }
}

DiskManager::~DiskManager()
{
    ZSTD_freeCCtx(static_cast<ZSTD_CCtx *>(cctx_));
    ZSTD_freeDCtx(static_cast<ZSTD_DCtx *>(dctx_));
    cctx_ = nullptr;
    dctx_ = nullptr;
    for (auto & [name, table] : tables_)
    {
        if (table.fd >= 0)
        {
            ::close(table.fd);
            table.fd = -1;
        }
    }
    if (wal_fd_ >= 0)
    {
        ::close(wal_fd_);
        wal_fd_ = -1;
    }
}

neon::base::Result<uint32_t> DiskManager::compress_page(const char * src)
{
    size_t n = ZSTD_compress2(static_cast<ZSTD_CCtx *>(cctx_), comp_buf_.data(), comp_buf_.size(), src, PAGE_SIZE);
    if (ZSTD_isError(n))
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), ZSTD_getErrorName(n)));
    }
    return static_cast<uint32_t>(n);
}

neon::base::Result<void> DiskManager::decompress_page(const char * src, size_t src_len, char * dest)
{
    size_t n = ZSTD_decompressDCtx(static_cast<ZSTD_DCtx *>(dctx_), dest, PAGE_SIZE, src, src_len);
    if (ZSTD_isError(n) || n != PAGE_SIZE)
    {
        return std::unexpected(neon::base::Error(
            static_cast<int32_t>(neon::base::InternalErr), ZSTD_isError(n) ? ZSTD_getErrorName(n) : "decompressed page size mismatch"));
    }
    return {};
}

std::string DiskManager::table_file_path(const std::string & table_name) const
{
    return data_dir_ + "/" + table_name + ".ndb";
}

DiskManager::TableState * DiskManager::find_table(const std::string & table_name)
{
    auto it = tables_.find(table_name);
    if (it == tables_.end())
    {
        return nullptr;
    }
    return &it->second;
}

const DiskManager::TableState * DiskManager::find_table(const std::string & table_name) const
{
    auto it = tables_.find(table_name);
    if (it == tables_.end())
    {
        return nullptr;
    }
    return &it->second;
}

neon::base::Result<void> DiskManager::read_block_meta(TableState & table, uint32_t block_id, BlockMeta & meta)
{
    std::memset(&meta, 0, sizeof(meta));
    return io_->read_at(table.fd, &meta, sizeof(meta), block_file_offset(block_id));
}

neon::base::Result<void> DiskManager::write_block_meta(TableState & table, uint32_t block_id)
{
    if (block_id >= table.blocks.size())
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "block id out of range"));
    }
    auto result = io_->write_at(table.fd, &table.blocks[block_id], sizeof(BlockMeta), block_file_offset(block_id));
    if (result)
    {
        table.block_dirty[block_id] = 0;
    }
    return result;
}

neon::base::Result<void> DiskManager::write_header(TableState & table)
{
    alignas(FileHeader) char buf[FILE_HEADER_SIZE];
    std::memset(buf, 0, sizeof(buf));
    std::memcpy(buf, &table.header, sizeof(FileHeader));
    return io_->write_at(table.fd, buf, FILE_HEADER_SIZE, 0);
}

neon::base::Result<void> DiskManager::load_table(const std::string & table_name, int fd)
{
    TableState table;
    table.fd = fd;
    auto header_read = io_->read_at(fd, &table.header, sizeof(FileHeader), 0);
    if (!header_read)
    {
        return header_read;
    }
    if (table.header.magic != FILE_MAGIC || table.header.version != FILE_VERSION || table.header.block_size != BLOCK_SIZE)
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "bad table file header: " + table_name));
    }

    table.blocks.resize(table.header.block_count);
    table.block_dirty.assign(table.header.block_count, 0);
    uint32_t reusable = NO_BLOCK;
    for (uint32_t id = 0; id < table.header.block_count; ++id)
    {
        BlockMeta meta;
        auto meta_read = read_block_meta(table, id, meta);
        if (!meta_read)
        {
            return meta_read;
        }
        if (meta.magic != BLOCK_MAGIC || meta.block_id != id)
        {
            meta = BlockMeta{};
            meta.magic = BLOCK_MAGIC;
            meta.block_id = id;
            meta.flags = BLOCK_FREE;
            meta.payload_end = BLOCK_META_SIZE;
        }

        uint16_t keep = 0;
        uint32_t end = BLOCK_META_SIZE;
        uint16_t raw_count = meta.page_count;
        if (raw_count > MAX_PAGES_PER_BLOCK)
        {
            raw_count = MAX_PAGES_PER_BLOCK;
        }
        for (uint16_t i = 0; i < raw_count; ++i)
        {
            const PageEntry & entry = meta.entries[i];
            if (entry.gen > table.header.committed_gen)
            {
                break;
            }
            if (i != keep)
            {
                meta.entries[keep] = entry;
            }
            if (entry.comp_size > 0)
            {
                end = entry.offset + entry.comp_size;
            }
            ++keep;
        }
        meta.page_count = keep;
        meta.payload_end = end;
        if (keep == 0)
        {
            meta.flags = BLOCK_FREE;
            table.free_blocks.push_back(id);
        }
        else if (block_has_space(meta, 1) || (meta.page_count < MAX_PAGES_PER_BLOCK && meta.payload_end < BLOCK_SIZE))
        {
            meta.flags = BLOCK_OPEN;
            reusable = id;
        }
        else
        {
            meta.flags = BLOCK_SEALED;
        }

        for (uint16_t i = 0; i < meta.page_count; ++i)
        {
            const PageEntry & entry = meta.entries[i];
            PageLoc loc;
            loc.block_id = id;
            loc.offset = entry.offset;
            loc.comp_size = entry.comp_size;
            loc.gen = entry.gen;
            loc.tombstone = entry.flags == ENTRY_TOMBSTONE;
            auto it = table.committed.find(entry.page_id);
            if (it == table.committed.end() || entry.gen >= it->second.gen)
            {
                table.committed[entry.page_id] = loc;
            }
        }
        table.blocks[id] = meta;
    }

    if (table.header.open_block < table.blocks.size() && table.blocks[table.header.open_block].flags == BLOCK_OPEN)
    {
        // keep header open block
    }
    else
    {
        table.header.open_block = reusable;
    }
    tables_.emplace(table_name, std::move(table));
    return {};
}

neon::base::Result<void> DiskManager::create_table_file(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = table_file_path(table_name);
    if (std::filesystem::exists(path) || tables_.contains(table_name))
    {
        return std::unexpected(
            neon::base::Error(static_cast<int32_t>(neon::base::TableAlreadyExists), "Table file already exists: " + path));
    }

    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "Failed to create table file: " + path));
    }
    if (::ftruncate(fd, FILE_HEADER_SIZE) != 0)
    {
        ::close(fd);
        std::filesystem::remove(path);
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "Failed to size table file: " + path));
    }

    TableState table;
    table.fd = fd;
    table.header.magic = FILE_MAGIC;
    table.header.version = FILE_VERSION;
    table.header.block_size = BLOCK_SIZE;
    table.header.block_count = 0;
    table.header.next_page_id = 1;
    table.header.open_block = NO_BLOCK;
    table.header.committed_gen = 0;
    auto written = write_header(table);
    if (!written)
    {
        ::close(fd);
        std::filesystem::remove(path);
        return written;
    }
    tables_.emplace(table_name, std::move(table));
    return {};
}

void DiskManager::remove_table_file(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(table_name);
    if (it != tables_.end())
    {
        if (it->second.fd >= 0)
        {
            ::close(it->second.fd);
        }
        tables_.erase(it);
    }
    std::string path = table_file_path(table_name);
    if (std::filesystem::exists(path))
    {
        std::filesystem::remove(path);
    }
}

const DiskManager::PageLoc * DiskManager::lookup(const TableState & table, page_id_t page_id) const
{
    if (auto it = table.overlay.find(page_id); it != table.overlay.end())
    {
        return &it->second;
    }
    if (auto it = table.committed.find(page_id); it != table.committed.end())
    {
        return &it->second;
    }
    return nullptr;
}

neon::base::Result<void> DiskManager::read_page(const std::string & table_name, page_id_t page_id, char * dest)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TableState * table = find_table(table_name);
    if (table == nullptr)
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "Table file does not exist: " + table_name));
    }
    const PageLoc * loc = lookup(*table, page_id);
    if (loc == nullptr || loc->tombstone || loc->comp_size == 0)
    {
        std::memset(dest, 0, PAGE_SIZE);
        return {};
    }
    if (auto pending = table->pending_pages.find(page_id); pending != table->pending_pages.end() && pending->second)
    {
        return decompress_page(pending->second->data(), pending->second->size(), dest);
    }
    if (auto cached = table->cached_pages.find(page_id); cached != table->cached_pages.end() && cached->second)
    {
        return decompress_page(cached->second->data(), cached->second->size(), dest);
    }
    std::vector<char> comp(loc->comp_size);
    uint64_t off = block_file_offset(loc->block_id) + loc->offset;
    auto read = io_->read_at(table->fd, comp.data(), loc->comp_size, off);
    if (!read)
    {
        return read;
    }
    return decompress_page(comp.data(), comp.size(), dest);
}

void DiskManager::touch(TableState & table)
{
    if (table.participating)
    {
        return;
    }
    table.participating = true;
    table.txn_gen = table.header.committed_gen + 1;
    table.snap.block_count = table.header.block_count;
    table.snap.next_page_id = table.header.next_page_id;
    table.snap.open_block = table.header.open_block;
    table.snap.free_blocks = table.free_blocks;
    table.snap.blocks = table.blocks;
    table.snap.block_dirty = table.block_dirty;
}

neon::base::Result<uint32_t> DiskManager::alloc_block(TableState & table)
{
    uint32_t id = NO_BLOCK;
    if (!table.free_blocks.empty())
    {
        id = table.free_blocks.back();
        table.free_blocks.pop_back();
    }
    else
    {
        id = table.header.block_count;
        uint32_t new_count = table.header.block_count + 1;
        off_t bytes = static_cast<off_t>(FILE_HEADER_SIZE) + static_cast<off_t>(new_count) * BLOCK_SIZE;
        if (::ftruncate(table.fd, bytes) != 0)
        {
            return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "ftruncate failed"));
        }
        table.grew = true;
        table.header.block_count = new_count;
        table.blocks.emplace_back();
        table.block_dirty.push_back(1);
    }

    BlockMeta meta{};
    meta.magic = BLOCK_MAGIC;
    meta.block_id = id;
    meta.flags = BLOCK_OPEN;
    meta.page_count = 0;
    meta.payload_end = BLOCK_META_SIZE;
    table.blocks[id] = meta;
    table.block_dirty[id] = 1;
    table.header.open_block = id;
    return id;
}

neon::base::Result<void> DiskManager::append_entry(
    TableState & table, page_id_t page_id, const char * comp, uint32_t comp_size, bool tombstone)
{
    touch(table);
    if (comp_size > BLOCK_SIZE - BLOCK_META_SIZE)
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "compressed page exceeds block payload"));
    }

    auto fits = [&](uint32_t id) {
        if (id >= table.blocks.size())
        {
            return false;
        }
        return block_has_space(table.blocks[id], comp_size);
    };

    uint32_t block_id = table.header.open_block;
    if (!fits(block_id))
    {
        if (block_id < table.blocks.size() && table.blocks[block_id].flags == BLOCK_OPEN)
        {
            table.blocks[block_id].flags = BLOCK_SEALED;
            table.block_dirty[block_id] = 1;
        }
        auto allocated = alloc_block(table);
        if (!allocated)
        {
            return std::unexpected(allocated.error());
        }
        block_id = allocated.value();
    }

    BlockMeta & meta = table.blocks[block_id];
    PageEntry & entry = meta.entries[meta.page_count];
    entry.page_id = page_id;
    entry.offset = meta.payload_end;
    entry.comp_size = comp_size;
    entry.flags = tombstone ? ENTRY_TOMBSTONE : ENTRY_DATA;
    entry.gen = table.txn_gen;

    PendingWrite pending;
    pending.offset = block_file_offset(block_id) + entry.offset;
    pending.page_id = page_id;
    pending.gen = table.txn_gen;
    if (comp_size > 0)
    {
        pending.bytes = std::make_shared<std::vector<char>>(comp, comp + comp_size);
        table.pending_pages[page_id] = pending.bytes;
        meta.payload_end += comp_size;
    }
    else
    {
        table.pending_pages.erase(page_id);
    }
    table.pending_io.push_back(std::move(pending));
    meta.page_count++;
    if (!block_has_space(meta, 1))
    {
        meta.flags = BLOCK_SEALED;
        if (table.header.open_block == block_id)
        {
            table.header.open_block = NO_BLOCK;
        }
    }
    table.block_dirty[block_id] = 1;

    PageLoc loc;
    loc.block_id = block_id;
    loc.offset = entry.offset;
    loc.comp_size = comp_size;
    loc.gen = table.txn_gen;
    loc.tombstone = tombstone;
    table.overlay[page_id] = loc;
    return {};
}

neon::base::Result<void> DiskManager::write_page(const std::string & table_name, page_id_t page_id, const char * src)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TableState * table = find_table(table_name);
    if (table == nullptr)
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "Table file does not exist: " + table_name));
    }
    auto comp_size = compress_page(src);
    if (!comp_size)
    {
        return std::unexpected(comp_size.error());
    }

    bool own = false;
    if (!in_txn_)
    {
        in_txn_ = true;
        own = true;
    }
    auto appended = append_entry(*table, page_id, comp_buf_.data(), comp_size.value(), false);
    if (!appended)
    {
        if (own)
        {
            abort_unlocked();
        }
        return appended;
    }
    if (own)
    {
        commit_unlocked();
    }
    return {};
}

neon::base::Result<page_id_t> DiskManager::allocate_page(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TableState * table = find_table(table_name);
    if (table == nullptr)
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "Table file does not exist: " + table_name));
    }
    if (in_txn_)
    {
        touch(*table);
    }
    page_id_t id = table->header.next_page_id++;
    return id;
}

page_id_t DiskManager::page_count(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const TableState * table = find_table(table_name);
    if (table == nullptr || table->header.next_page_id == 0)
    {
        return 0;
    }
    return table->header.next_page_id - 1;
}

bool DiskManager::table_file_exists(const std::string & table_name) const
{
    return std::filesystem::exists(table_file_path(table_name));
}

bool DiskManager::begin_txn()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (in_txn_)
    {
        return false;
    }
    in_txn_ = true;
    return true;
}

bool DiskManager::in_txn() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return in_txn_;
}

void DiskManager::commit_txn()
{
    std::lock_guard<std::mutex> lock(mutex_);
    commit_unlocked();
}

void DiskManager::abort_txn()
{
    std::lock_guard<std::mutex> lock(mutex_);
    abort_unlocked();
}

void DiskManager::retire_page(const std::string & table_name, page_id_t page_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TableState * table = find_table(table_name);
    if (table == nullptr)
    {
        return;
    }
    const PageLoc * loc = lookup(*table, page_id);
    if (loc == nullptr || loc->tombstone)
    {
        return;
    }
    bool own = false;
    if (!in_txn_)
    {
        in_txn_ = true;
        own = true;
    }
    auto appended = append_entry(*table, page_id, nullptr, 0, true);
    if (!appended)
    {
        if (own)
        {
            abort_unlocked();
        }
        return;
    }
    if (own)
    {
        commit_unlocked();
    }
}

void DiskManager::open_wal()
{
    std::string path = data_dir_ + "/commit.wal";
    wal_fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (wal_fd_ < 0)
    {
        throw std::runtime_error("failed to open commit log: " + path);
    }
    struct stat st{};
    if (::fstat(wal_fd_, &st) != 0)
    {
        throw std::runtime_error("failed to stat commit log");
    }
    if (st.st_size < static_cast<off_t>(FILE_HEADER_SIZE))
    {
        if (::ftruncate(wal_fd_, FILE_HEADER_SIZE) != 0)
        {
            throw std::runtime_error("failed to size commit log");
        }
        alignas(8) char buf[FILE_HEADER_SIZE];
        std::memset(buf, 0, sizeof(buf));
        uint32_t magic = WAL_MAGIC;
        uint32_t version = WAL_VERSION;
        std::memcpy(buf, &magic, sizeof(magic));
        std::memcpy(buf + sizeof(magic), &version, sizeof(version));
        auto written = io_->write_at(wal_fd_, buf, FILE_HEADER_SIZE, 0);
        if (!written)
        {
            throw std::runtime_error(written.error().error_message);
        }
        auto synced = io_->fsync(wal_fd_);
        if (!synced)
        {
            throw std::runtime_error(synced.error().error_message);
        }
        wal_end_ = FILE_HEADER_SIZE;
        wal_alloc_ = FILE_HEADER_SIZE;
        return;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    auto header = io_->read_at(wal_fd_, &magic, sizeof(magic), 0);
    if (!header)
    {
        throw std::runtime_error(header.error().error_message);
    }
    header = io_->read_at(wal_fd_, &version, sizeof(version), sizeof(magic));
    if (!header)
    {
        throw std::runtime_error(header.error().error_message);
    }
    if (magic != WAL_MAGIC || version != WAL_VERSION)
    {
        throw std::runtime_error("bad commit log header");
    }
    wal_alloc_ = static_cast<uint64_t>(st.st_size);
    replay_wal();
}

void DiskManager::replay_wal()
{
    struct stat st{};
    if (::fstat(wal_fd_, &st) != 0)
    {
        throw std::runtime_error("failed to stat commit log");
    }
    uint64_t file_size = static_cast<uint64_t>(st.st_size);
    uint64_t pos = FILE_HEADER_SIZE;
    std::unordered_map<std::string, int> fds;
    auto close_fds = [&]() {
        for (auto & [name, fd] : fds)
        {
            if (fd >= 0)
            {
                ::close(fd);
            }
        }
        fds.clear();
    };

    while (pos + 12 <= file_size)
    {
        uint32_t magic = 0;
        uint32_t len = 0;
        uint32_t crc = 0;
        auto prefix = io_->read_at(wal_fd_, &magic, sizeof(magic), pos);
        if (!prefix)
        {
            close_fds();
            throw std::runtime_error(prefix.error().error_message);
        }
        if (magic == 0)
        {
            break;
        }
        if (magic != WAL_REC_MAGIC)
        {
            break;
        }
        prefix = io_->read_at(wal_fd_, &len, sizeof(len), pos + 4);
        if (!prefix)
        {
            close_fds();
            throw std::runtime_error(prefix.error().error_message);
        }
        prefix = io_->read_at(wal_fd_, &crc, sizeof(crc), pos + 8);
        if (!prefix)
        {
            close_fds();
            throw std::runtime_error(prefix.error().error_message);
        }
        if (len > WAL_REC_LIMIT || pos + 12 + static_cast<uint64_t>(len) > file_size)
        {
            break;
        }
        std::vector<char> payload(len);
        if (len > 0)
        {
            auto body = io_->read_at(wal_fd_, payload.data(), len, pos + 12);
            if (!body)
            {
                close_fds();
                throw std::runtime_error(body.error().error_message);
            }
            if (wal_crc(payload.data(), payload.size()) != crc)
            {
                break;
            }
        }
        else if (wal_crc(nullptr, 0) != crc)
        {
            break;
        }

        struct Piece
        {
            uint64_t offset = 0;
            std::vector<char> bytes;
        };
        struct ApplyTable
        {
            std::string name;
            FileHeader header{};
            std::vector<std::pair<uint32_t, BlockMeta>> metas;
            std::vector<Piece> pieces;
        };
        WalCursor cursor{payload.data(), payload.size(), 0, true};
        uint32_t table_count = cursor.take<uint32_t>();
        std::vector<ApplyTable> batch;
        batch.reserve(table_count);
        for (uint32_t t = 0; cursor.ok && t < table_count; ++t)
        {
            ApplyTable item;
            uint32_t name_len = cursor.take<uint32_t>();
            item.name = cursor.take_name(name_len);
            item.header = cursor.take<FileHeader>();
            uint32_t meta_count = cursor.take<uint32_t>();
            item.metas.reserve(meta_count);
            for (uint32_t m = 0; cursor.ok && m < meta_count; ++m)
            {
                uint32_t block_id = cursor.take<uint32_t>();
                BlockMeta meta = cursor.take<BlockMeta>();
                item.metas.emplace_back(block_id, meta);
            }
            uint32_t write_count = cursor.take<uint32_t>();
            item.pieces.reserve(write_count);
            for (uint32_t w = 0; cursor.ok && w < write_count; ++w)
            {
                Piece piece;
                piece.offset = cursor.take<uint64_t>();
                uint32_t piece_len = cursor.take<uint32_t>();
                if (!cursor.ok || cursor.pos + piece_len > cursor.size)
                {
                    cursor.ok = false;
                    break;
                }
                piece.bytes.assign(cursor.data + cursor.pos, cursor.data + cursor.pos + piece_len);
                cursor.pos += piece_len;
                item.pieces.push_back(std::move(piece));
            }
            if (!cursor.ok)
            {
                break;
            }
            batch.push_back(std::move(item));
        }
        if (!cursor.ok || cursor.pos != cursor.size || batch.size() != table_count)
        {
            break;
        }
        for (const ApplyTable & item : batch)
        {
            int fd = -1;
            if (auto it = fds.find(item.name); it != fds.end())
            {
                fd = it->second;
            }
            else
            {
                std::string path = table_file_path(item.name);
                fd = ::open(path.c_str(), O_RDWR);
                if (fd < 0)
                {
                    close_fds();
                    throw std::runtime_error("commit log references missing table file: " + path);
                }
                fds.emplace(item.name, fd);
            }
            off_t need = static_cast<off_t>(FILE_HEADER_SIZE) + static_cast<off_t>(item.header.block_count) * static_cast<off_t>(BLOCK_SIZE);
            struct stat table_st{};
            if (::fstat(fd, &table_st) != 0)
            {
                close_fds();
                throw std::runtime_error("failed to stat table file during recovery");
            }
            if (table_st.st_size < need && ::ftruncate(fd, need) != 0)
            {
                close_fds();
                throw std::runtime_error("failed to extend table file during recovery");
            }
            for (const Piece & piece : item.pieces)
            {
                auto written = io_->write_at(fd, piece.bytes.data(), static_cast<uint32_t>(piece.bytes.size()), piece.offset);
                if (!written)
                {
                    close_fds();
                    throw std::runtime_error(written.error().error_message);
                }
            }
            for (const auto & [block_id, meta] : item.metas)
            {
                auto written = io_->write_at(fd, &meta, sizeof(meta), block_file_offset(block_id));
                if (!written)
                {
                    close_fds();
                    throw std::runtime_error(written.error().error_message);
                }
            }
            alignas(8) char header_buf[FILE_HEADER_SIZE];
            std::memset(header_buf, 0, sizeof(header_buf));
            std::memcpy(header_buf, &item.header, sizeof(item.header));
            auto written = io_->write_at(fd, header_buf, FILE_HEADER_SIZE, 0);
            if (!written)
            {
                close_fds();
                throw std::runtime_error(written.error().error_message);
            }
        }
        pos += 12 + static_cast<uint64_t>(len);
    }

    for (auto & [name, fd] : fds)
    {
        auto synced = io_->fsync(fd);
        if (!synced)
        {
            close_fds();
            throw std::runtime_error(synced.error().error_message);
        }
    }
    close_fds();
    reset_wal();
}

void DiskManager::reset_wal()
{
    if (::ftruncate(wal_fd_, FILE_HEADER_SIZE) != 0)
    {
        throw std::runtime_error("failed to truncate commit log");
    }
    wal_end_ = FILE_HEADER_SIZE;
    wal_alloc_ = FILE_HEADER_SIZE;
    auto synced = io_->fsync(wal_fd_);
    if (!synced)
    {
        throw std::runtime_error(synced.error().error_message);
    }
}

neon::base::Result<void> DiskManager::durable_wal_write(const char * data, uint32_t len)
{
    uint64_t need = wal_end_ + len;
    bool grew = false;
    if (need > wal_alloc_)
    {
        uint64_t alloc = wal_alloc_ == 0 ? WAL_CHUNK : wal_alloc_;
        while (alloc < need)
        {
            alloc += WAL_CHUNK;
        }
        if (::ftruncate(wal_fd_, static_cast<off_t>(alloc)) != 0)
        {
            return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "failed to extend commit log"));
        }
        wal_alloc_ = alloc;
        grew = true;
    }
    auto written = io_->write_at(wal_fd_, data, len, wal_end_);
    if (!written)
    {
        return written;
    }
    auto synced = grew ? io_->fsync(wal_fd_) : io_->sync_writes(wal_fd_, nullptr, 0, true);
    if (!synced)
    {
        return synced;
    }
    wal_end_ += len;
    return {};
}

neon::base::Result<void> DiskManager::append_wal_commit()
{
    wal_payload_.clear();
    if (wal_payload_.capacity() < 64 * 1024)
    {
        wal_payload_.reserve(64 * 1024);
    }
    std::vector<char> & payload = wal_payload_;
    uint32_t table_count = 0;
    for (const auto & [name, table] : tables_)
    {
        if (table.participating)
        {
            ++table_count;
        }
    }
    append_pod(payload, table_count);
    for (const auto & [name, table] : tables_)
    {
        if (!table.participating)
        {
            continue;
        }
        uint32_t name_len = static_cast<uint32_t>(name.size());
        append_pod(payload, name_len);
        append_raw(payload, name.data(), name.size());
        FileHeader header = table.header;
        if (table.txn_gen != 0)
        {
            header.committed_gen = table.txn_gen;
        }
        append_pod(payload, header);
        uint32_t meta_count = 0;
        for (uint32_t id = 0; id < table.blocks.size(); ++id)
        {
            if (id < table.block_dirty.size() && table.block_dirty[id])
            {
                ++meta_count;
            }
        }
        append_pod(payload, meta_count);
        for (uint32_t id = 0; id < table.blocks.size(); ++id)
        {
            if (id < table.block_dirty.size() && table.block_dirty[id])
            {
                append_pod(payload, id);
                append_pod(payload, table.blocks[id]);
            }
        }
        uint32_t write_count = 0;
        for (const PendingWrite & pending : table.pending_io)
        {
            if (pending.bytes && !pending.bytes->empty())
            {
                ++write_count;
            }
        }
        append_pod(payload, write_count);
        for (const PendingWrite & pending : table.pending_io)
        {
            if (!pending.bytes || pending.bytes->empty())
            {
                continue;
            }
            append_pod(payload, pending.offset);
            uint32_t piece_len = static_cast<uint32_t>(pending.bytes->size());
            append_pod(payload, piece_len);
            append_raw(payload, pending.bytes->data(), pending.bytes->size());
        }
    }

    if (payload.size() > WAL_REC_LIMIT)
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "commit record exceeds limit"));
    }
    wal_record_.clear();
    wal_record_.reserve(12 + payload.size());
    uint32_t magic = WAL_REC_MAGIC;
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint32_t crc = wal_crc(payload.data(), payload.size());
    append_pod(wal_record_, magic);
    append_pod(wal_record_, len);
    append_pod(wal_record_, crc);
    append_raw(wal_record_, payload.data(), payload.size());
    return durable_wal_write(wal_record_.data(), static_cast<uint32_t>(wal_record_.size()));
}

void DiskManager::publish_memory(TableState & table)
{
    for (const PendingWrite & pending : table.pending_io)
    {
        if (!pending.bytes || pending.bytes->empty())
        {
            continue;
        }
        auto written = io_->write_at(table.fd, pending.bytes->data(), static_cast<uint32_t>(pending.bytes->size()), pending.offset);
        if (!written)
        {
            throw std::runtime_error(written.error().error_message);
        }
    }
    for (uint32_t id = 0; id < table.blocks.size(); ++id)
    {
        if (id < table.block_dirty.size() && table.block_dirty[id])
        {
            auto written = io_->write_at(table.fd, &table.blocks[id], sizeof(BlockMeta), block_file_offset(id));
            if (!written)
            {
                throw std::runtime_error(written.error().error_message);
            }
            table.block_dirty[id] = 0;
        }
    }
    if (table.txn_gen != 0)
    {
        table.header.committed_gen = table.txn_gen;
    }
    auto header = write_header(table);
    if (!header)
    {
        throw std::runtime_error(header.error().error_message);
    }
    for (const auto & [pid, loc] : table.overlay)
    {
        table.committed[pid] = loc;
    }
    table.overlay.clear();
    table.pending_io.clear();
    table.pending_pages.clear();
    table.participating = false;
    table.txn_gen = 0;
    table.grew = false;
}

void DiskManager::commit_unlocked()
{
    if (!in_txn_)
    {
        return;
    }
    std::vector<TableState *> published;
    for (auto & [name, table] : tables_)
    {
        if (table.participating)
        {
            published.push_back(&table);
        }
    }
    if (!published.empty())
    {
        auto logged = append_wal_commit();
        if (!logged)
        {
            spdlog::error("DiskManager::commit: {}", logged.error().error_message);
            throw std::runtime_error(logged.error().error_message);
        }
        for (TableState * table : published)
        {
            publish_memory(*table);
        }
    }
    in_txn_ = false;
    if (!in_gc_)
    {
        for (TableState * table : published)
        {
            maybe_gc(*table);
        }
    }
}

void DiskManager::abort_table(TableState & table)
{
    if (!table.participating)
    {
        return;
    }
    off_t bytes = static_cast<off_t>(FILE_HEADER_SIZE) + static_cast<off_t>(table.snap.block_count) * BLOCK_SIZE;
    if (bytes < FILE_HEADER_SIZE)
    {
        bytes = FILE_HEADER_SIZE;
    }
    ::ftruncate(table.fd, bytes);
    table.header.block_count = table.snap.block_count;
    table.header.next_page_id = table.snap.next_page_id;
    table.header.open_block = table.snap.open_block;
    table.blocks = table.snap.blocks;
    table.block_dirty = table.snap.block_dirty;
    table.free_blocks = table.snap.free_blocks;
    table.overlay.clear();
    table.pending_io.clear();
    table.pending_pages.clear();
    table.grew = false;
    table.participating = false;
    table.txn_gen = 0;
}

void DiskManager::abort_unlocked()
{
    if (!in_txn_)
    {
        return;
    }
    for (auto & [name, table] : tables_)
    {
        abort_table(table);
    }
    in_txn_ = false;
}

uint64_t DiskManager::dead_bytes(const TableState & table) const
{
    uint64_t used = 0;
    for (const BlockMeta & meta : table.blocks)
    {
        if (meta.flags == BLOCK_FREE || meta.payload_end <= BLOCK_META_SIZE)
        {
            continue;
        }
        used += meta.payload_end - BLOCK_META_SIZE;
    }
    uint64_t live = 0;
    auto add_live = [&](const std::unordered_map<page_id_t, PageLoc> & map) {
        for (const auto & [pid, loc] : map)
        {
            if (!loc.tombstone)
            {
                live += loc.comp_size;
            }
        }
    };
    add_live(table.committed);
    for (const auto & [pid, loc] : table.overlay)
    {
        if (auto it = table.committed.find(pid); it != table.committed.end() && !it->second.tombstone)
        {
            if (live >= it->second.comp_size)
            {
                live -= it->second.comp_size;
            }
        }
        if (!loc.tombstone)
        {
            live += loc.comp_size;
        }
    }
    return used > live ? used - live : 0;
}

void DiskManager::maybe_gc(TableState & table)
{
    if (in_txn_ || in_gc_)
    {
        return;
    }
    if (dead_bytes(table) < GC_WASTE_THRESHOLD)
    {
        return;
    }
    in_gc_ = true;
    auto result = gc_table(table);
    in_gc_ = false;
    if (!result)
    {
        spdlog::error("DiskManager::gc: {}", result.error().error_message);
    }
}

neon::base::Result<void> DiskManager::gc_table(TableState & table)
{
    struct Live
    {
        page_id_t page_id = INVALID_PAGE_ID;
        std::vector<char> bytes;
    };
    std::vector<Live> lives;
    lives.reserve(table.committed.size());
    for (const auto & [pid, loc] : table.committed)
    {
        if (loc.tombstone || loc.comp_size == 0)
        {
            continue;
        }
        Live live;
        live.page_id = pid;
        if (auto pending = table.pending_pages.find(pid); pending != table.pending_pages.end() && pending->second)
        {
            live.bytes = *pending->second;
        }
        else if (auto cached = table.cached_pages.find(pid); cached != table.cached_pages.end() && cached->second)
        {
            live.bytes = *cached->second;
        }
        else
        {
            live.bytes.resize(loc.comp_size);
            uint64_t off = block_file_offset(loc.block_id) + loc.offset;
            auto read = io_->read_at(table.fd, live.bytes.data(), loc.comp_size, off);
            if (!read)
            {
                return read;
            }
        }
        lives.push_back(std::move(live));
    }

    std::unordered_set<uint32_t> sources;
    for (uint32_t id = 0; id < table.blocks.size(); ++id)
    {
        if (table.blocks[id].flags != BLOCK_FREE)
        {
            sources.insert(id);
        }
    }

    in_txn_ = true;
    touch(table);
    table.header.open_block = NO_BLOCK;
    for (const Live & live : lives)
    {
        auto appended = append_entry(table, live.page_id, live.bytes.data(), static_cast<uint32_t>(live.bytes.size()), false);
        if (!appended)
        {
            abort_unlocked();
            return appended;
        }
    }

    std::unordered_set<uint32_t> kept;
    for (const auto & [pid, loc] : table.overlay)
    {
        kept.insert(loc.block_id);
    }
    for (uint32_t id : sources)
    {
        if (kept.contains(id))
        {
            continue;
        }
        BlockMeta meta{};
        meta.magic = BLOCK_MAGIC;
        meta.block_id = id;
        meta.flags = BLOCK_FREE;
        meta.payload_end = BLOCK_META_SIZE;
        table.blocks[id] = meta;
        table.block_dirty[id] = 1;
        table.free_blocks.push_back(id);
    }
    if (table.header.open_block < table.blocks.size() && sources.contains(table.header.open_block) && !kept.contains(table.header.open_block))
    {
        table.header.open_block = NO_BLOCK;
    }
    commit_unlocked();

    for (auto it = table.committed.begin(); it != table.committed.end();)
    {
        if (it->second.tombstone || sources.contains(it->second.block_id))
        {
            it = table.committed.erase(it);
        }
        else
        {
            ++it;
        }
    }
    spdlog::info("DiskManager::gc reclaimed {} blocks, live pages {}", sources.size(), table.committed.size());
    return {};
}

} // namespace neon::storage
