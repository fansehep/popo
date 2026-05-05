#include "src/storage/lru_replacer.h"

namespace neon::storage
{

LRUReplacer::LRUReplacer(size_t capacity) : capacity_(capacity)
{
}

void LRUReplacer::record_access(frame_id_t frame_id)
{
    std::lock_guard lock(mutex_);
    if (auto it = map_.find(frame_id); it != map_.end())
    {
        // Already tracked – move to MRU end (back).
        lru_list_.erase(it->second);
        lru_list_.push_back(frame_id);
        it->second = std::prev(lru_list_.end());
        return;
    }
    // Not tracked yet – add only if under capacity.
    if (lru_list_.size() >= capacity_)
    {
        return;
    }
    lru_list_.push_back(frame_id);
    map_[frame_id] = std::prev(lru_list_.end());
}

void LRUReplacer::pin(frame_id_t frame_id)
{
    std::lock_guard lock(mutex_);
    if (auto it = map_.find(frame_id); it != map_.end())
    {
        lru_list_.erase(it->second);
        map_.erase(it);
    }
}

void LRUReplacer::unpin(frame_id_t frame_id)
{
    std::lock_guard lock(mutex_);
    // Already tracked – nothing to do.
    if (map_.contains(frame_id))
    {
        return;
    }
    // Don't exceed capacity.
    if (lru_list_.size() >= capacity_)
    {
        return;
    }
    lru_list_.push_back(frame_id);
    map_[frame_id] = std::prev(lru_list_.end());
}

std::optional<frame_id_t> LRUReplacer::evict()
{
    std::lock_guard lock(mutex_);
    if (lru_list_.empty())
    {
        return std::nullopt;
    }
    frame_id_t victim = lru_list_.front();
    lru_list_.pop_front();
    map_.erase(victim);
    return victim;
}

size_t LRUReplacer::size() const
{
    std::lock_guard lock(mutex_);
    return lru_list_.size();
}

} // namespace neon::storage
