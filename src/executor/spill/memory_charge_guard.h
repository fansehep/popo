#pragma once

#include <cstddef>

#include "executor/spill/memory_tracker.h"

namespace popo::executor
{

// RAII accumulator for incremental reservations charged to a MemoryTracker.
class MemoryChargeGuard
{
public:
    explicit MemoryChargeGuard(MemoryTracker * tracker) noexcept : tracker_(tracker) { }
    ~MemoryChargeGuard() { release(); }

    MemoryChargeGuard(const MemoryChargeGuard &) = delete;
    MemoryChargeGuard & operator=(const MemoryChargeGuard &) = delete;

    bool charge(size_t bytes) noexcept
    {
        if (!tracker_ || !tracker_->try_reserve(bytes))
            return false;
        charged_ += bytes;
        return true;
    }

    void adopt(size_t bytes) noexcept { charged_ += bytes; }

    void release() noexcept
    {
        if (tracker_ && charged_ > 0)
            tracker_->release(charged_);
        charged_ = 0;
    }

    size_t charged() const noexcept { return charged_; }

private:
    MemoryTracker * tracker_ = nullptr;
    size_t charged_ = 0;
};

} // namespace popo::executor
