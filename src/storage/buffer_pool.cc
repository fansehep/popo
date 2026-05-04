#include "src/storage/buffer_pool.h"

#include <cstring>

namespace neon::storage {

// ---------------------------------------------------------------------------
// PageGuard
// ---------------------------------------------------------------------------

PageGuard::PageGuard(BufferPoolManager* pool, frame_id_t frame_id)
    : pool_(pool), frame_id_(frame_id) {}

PageGuard::~PageGuard() {
    if (pool_ != nullptr && frame_id_ != INVALID_FRAME_ID) {
        pool_->unpin(frame_id_, false);
    }
}

PageGuard::PageGuard(PageGuard&& other) noexcept
    : pool_(other.pool_), frame_id_(other.frame_id_) {
    other.pool_ = nullptr;
    other.frame_id_ = INVALID_FRAME_ID;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
    if (this != &other) {
        // Release current resource first.
        if (pool_ != nullptr && frame_id_ != INVALID_FRAME_ID) {
            pool_->unpin(frame_id_, false);
        }
        pool_ = other.pool_;
        frame_id_ = other.frame_id_;
        other.pool_ = nullptr;
        other.frame_id_ = INVALID_FRAME_ID;
    }
    return *this;
}

char* PageGuard::data() {
    return pool_->get_frame_data(frame_id_);
}

const char* PageGuard::data() const {
    return pool_->get_frame_data(frame_id_);
}

void PageGuard::mark_dirty() {
    pool_->mark_frame_dirty(frame_id_);
}

page_id_t PageGuard::page_id() const {
    return pool_->get_frame_page_id(frame_id_);
}

bool PageGuard::is_valid() const {
    return pool_ != nullptr && frame_id_ != INVALID_FRAME_ID;
}

// ---------------------------------------------------------------------------
// BufferPoolManager
// ---------------------------------------------------------------------------

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk_manager)
    : pool_size_(pool_size),
      disk_manager_(disk_manager),
      frames_(pool_size),
      replacer_(pool_size) {
    for (size_t i = 0; i < pool_size; ++i) {
        free_list_.push_back(static_cast<frame_id_t>(i));
    }
}

BufferPoolManager::~BufferPoolManager() {
    flush_all();
}

PageGuard BufferPoolManager::fetch_page(const std::string& table_name,
                                        page_id_t page_id) {
    std::lock_guard lock(latch_);

    PageKey key{table_name, page_id};

    // Check if already in buffer pool.
    if (auto it = page_table_.find(key); it != page_table_.end()) {
        frame_id_t fid = it->second;
        frames_[fid].pin_count++;
        replacer_.pin(fid);
        return PageGuard(this, fid);
    }

    // Not cached – need a free frame.
    auto frame_opt = get_free_frame();
    if (!frame_opt.has_value()) {
        return PageGuard();  // invalid guard
    }

    frame_id_t fid = frame_opt.value();
    FrameInfo& frame = frames_[fid];
    frame.key = key;
    frame.is_dirty = false;
    frame.pin_count = 1;
    frame.page_lsn = INVALID_LSN;

    // Read page data from disk.
    disk_manager_->read_page(table_name, page_id, frame.data);

    page_table_[key] = fid;
    replacer_.pin(fid);

    return PageGuard(this, fid);
}

PageGuard BufferPoolManager::new_page(const std::string& table_name,
                                      page_id_t& out_page_id) {
    std::lock_guard lock(latch_);

    auto frame_opt = get_free_frame();
    if (!frame_opt.has_value()) {
        out_page_id = INVALID_PAGE_ID;
        return PageGuard();
    }

    frame_id_t fid = frame_opt.value();
    page_id_t new_pid = disk_manager_->allocate_page(table_name);

    FrameInfo& frame = frames_[fid];
    frame.key = PageKey{table_name, new_pid};
    frame.is_dirty = false;
    frame.pin_count = 1;
    frame.page_lsn = INVALID_LSN;
    std::memset(frame.data, 0, PAGE_SIZE);

    page_table_[frame.key] = fid;
    replacer_.pin(fid);

    out_page_id = new_pid;
    return PageGuard(this, fid);
}

bool BufferPoolManager::flush_page(const std::string& table_name,
                                   page_id_t page_id) {
    std::lock_guard lock(latch_);

    PageKey key{table_name, page_id};
    auto it = page_table_.find(key);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t fid = it->second;
    FrameInfo& frame = frames_[fid];
    if (frame.is_dirty) {
        disk_manager_->write_page(table_name, page_id, frame.data);
        frame.is_dirty = false;
    }
    return true;
}

void BufferPoolManager::flush_all() {
    std::lock_guard lock(latch_);

    for (size_t i = 0; i < pool_size_; ++i) {
        FrameInfo& frame = frames_[i];
        if (frame.is_dirty) {
            disk_manager_->write_page(frame.key.table_name, frame.key.page_id,
                                      frame.data);
            frame.is_dirty = false;
        }
    }
}

void BufferPoolManager::unpin(frame_id_t frame_id, bool is_dirty) {
    std::lock_guard lock(latch_);

    FrameInfo& frame = frames_[frame_id];
    frame.is_dirty = frame.is_dirty || is_dirty;

    if (frame.pin_count <= 0) {
        return;
    }
    frame.pin_count--;
    if (frame.pin_count == 0) {
        replacer_.unpin(frame_id);
    }
}

char* BufferPoolManager::get_frame_data(frame_id_t frame_id) {
    return frames_[frame_id].data;
}

const char* BufferPoolManager::get_frame_data(frame_id_t frame_id) const {
    return frames_[frame_id].data;
}

page_id_t BufferPoolManager::get_frame_page_id(frame_id_t frame_id) const {
    return frames_[frame_id].key.page_id;
}

void BufferPoolManager::mark_frame_dirty(frame_id_t frame_id) {
    std::lock_guard lock(latch_);
    frames_[frame_id].is_dirty = true;
}

std::optional<frame_id_t> BufferPoolManager::get_free_frame() {
    // Try free list first.
    if (!free_list_.empty()) {
        frame_id_t fid = free_list_.front();
        free_list_.pop_front();
        return fid;
    }

    // Try eviction.
    auto victim_opt = replacer_.evict();
    if (!victim_opt.has_value()) {
        return std::nullopt;
    }

    frame_id_t fid = victim_opt.value();
    FrameInfo& frame = frames_[fid];

    // Flush dirty victim to disk.
    if (frame.is_dirty) {
        disk_manager_->write_page(frame.key.table_name, frame.key.page_id,
                                  frame.data);
        frame.is_dirty = false;
    }

    // Remove old mapping.
    page_table_.erase(frame.key);

    return fid;
}

}  // namespace neon::storage
