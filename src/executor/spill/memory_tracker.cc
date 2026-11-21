#include "executor/spill/memory_tracker.h"

#include <algorithm>

namespace popo::executor
{

bool MemoryTracker::try_reserve(size_t bytes) noexcept
{
    size_t current = used_bytes_.load(std::memory_order_relaxed);
    for (;;)
    {
        if (bytes > limit_bytes_ || current > limit_bytes_ - bytes)
            return false;
        if (used_bytes_.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed))
        {
            size_t peak = peak_bytes_.load(std::memory_order_relaxed);
            while (peak < current + bytes
                   && !peak_bytes_.compare_exchange_weak(peak, current + bytes, std::memory_order_relaxed))
            {
            }
            return true;
        }
    }
}

void MemoryTracker::release(size_t bytes) noexcept
{
    used_bytes_.fetch_sub(std::min(bytes, used()), std::memory_order_relaxed);
}

} // namespace popo::executor
