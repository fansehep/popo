#pragma once

#include "src/storage/common.h"
#include "src/storage/disk_manager.h"
#include "src/storage/lru_replacer.h"

#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace neon::storage
{

// Composite key for buffer pool: (table_name, page_id)
struct PageKey
{
    std::string table_name;
    page_id_t page_id = INVALID_PAGE_ID;

    bool operator==(const PageKey & other) const { return table_name == other.table_name && page_id == other.page_id; }
};

struct PageKeyHash
{
    size_t operator()(const PageKey & k) const
    {
        auto h1 = std::hash<std::string>{}(k.table_name);
        auto h2 = std::hash<page_id_t>{}(k.page_id);
        return h1 ^ (h2 << 1);
    }
};

// Internal frame metadata
struct FrameInfo
{
    PageKey key;
    char data[PAGE_SIZE]{};
    bool is_dirty = false;
    int32_t pin_count = 0;
    lsn_t page_lsn = INVALID_LSN;
};

class BufferPoolManager;

// RAII page guard – automatically unpins page on destruction
class PageGuard
{
public:
    PageGuard() = default;
    PageGuard(BufferPoolManager * pool, frame_id_t frame_id);
    ~PageGuard();

    // Move only
    PageGuard(PageGuard && other) noexcept;
    PageGuard & operator=(PageGuard && other) noexcept;
    PageGuard(const PageGuard &) = delete;
    PageGuard & operator=(const PageGuard &) = delete;

    char * data();
    const char * data() const;
    void mark_dirty();
    page_id_t page_id() const;
    bool is_valid() const;

private:
    BufferPoolManager * pool_ = nullptr;
    frame_id_t frame_id_ = INVALID_FRAME_ID;
};

class BufferPoolManager
{
public:
    BufferPoolManager(size_t pool_size, DiskManager * disk_manager);
    ~BufferPoolManager();

    /// Fetch an existing page from buffer pool (reads from disk if not cached).
    PageGuard fetch_page(const std::string & table_name, page_id_t page_id);

    /// Allocate a new page (extends file) and bring into buffer pool.
    PageGuard new_page(const std::string & table_name, page_id_t & out_page_id);

    /// Flush a specific page to disk.
    bool flush_page(const std::string & table_name, page_id_t page_id);

    /// Flush all dirty pages to disk.
    void flush_all();

    /// Unpin a frame (called by PageGuard destructor).
    void unpin(frame_id_t frame_id, bool is_dirty);

    /// Get frame data pointer (for PageGuard).
    char * get_frame_data(frame_id_t frame_id);
    const char * get_frame_data(frame_id_t frame_id) const;
    page_id_t get_frame_page_id(frame_id_t frame_id) const;
    void mark_frame_dirty(frame_id_t frame_id);

    DiskManager * disk_manager() { return disk_manager_; }

private:
    size_t pool_size_;
    DiskManager * disk_manager_;
    std::vector<FrameInfo> frames_;
    LRUReplacer replacer_;
    std::unordered_map<PageKey, frame_id_t, PageKeyHash> page_table_;
    std::list<frame_id_t> free_list_;
    std::mutex latch_;

    // Get a free frame (from free list or by evicting).
    std::optional<frame_id_t> get_free_frame();
};

} // namespace neon::storage
