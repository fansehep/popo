#pragma once

#include "src/storage/common.h"

#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace neon::storage {

class LRUReplacer {
public:
    explicit LRUReplacer(size_t capacity);

    /// Record that frame was accessed (move to most-recently-used end).
    /// If frame not yet tracked, add it.
    void record_access(frame_id_t frame_id);

    /// Remove frame from replacer (it's been pinned).
    void pin(frame_id_t frame_id);

    /// Make frame eligible for eviction (it's been unpinned).
    void unpin(frame_id_t frame_id);

    /// Evict the least-recently-used frame. Returns nullopt if no evictable frame.
    std::optional<frame_id_t> evict();

    /// Number of evictable frames.
    size_t size() const;

private:
    size_t capacity_;
    // Doubly-linked list: front = LRU (evict first), back = MRU
    std::list<frame_id_t> lru_list_;
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> map_;
    mutable std::mutex mutex_;
};

}  // namespace neon::storage
