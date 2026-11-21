#pragma once

#include <atomic>
#include <cstddef>

namespace popo::executor
{

// Query-scoped accounting. Operators reserve only resident data; spill files are
// deliberately not charged to the memory budget.
class MemoryTracker
{
public:
    explicit MemoryTracker(size_t limit_bytes) : limit_bytes_(limit_bytes) { }

    bool try_reserve(size_t bytes) noexcept;
    void release(size_t bytes) noexcept;
    size_t used() const noexcept { return used_bytes_.load(std::memory_order_relaxed); }
    size_t peak() const noexcept { return peak_bytes_.load(std::memory_order_relaxed); }
    size_t limit() const noexcept { return limit_bytes_; }

private:
    size_t limit_bytes_;
    std::atomic<size_t> used_bytes_{0};
    std::atomic<size_t> peak_bytes_{0};
};

} // namespace popo::executor
