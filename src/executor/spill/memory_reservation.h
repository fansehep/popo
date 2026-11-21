#pragma once

#include <cstddef>

#include "executor/spill/memory_tracker.h"

namespace popo::executor
{

class MemoryReservation
{
public:
    MemoryReservation() = default;
    MemoryReservation(MemoryTracker & tracker, size_t bytes);
    ~MemoryReservation();
    MemoryReservation(MemoryReservation && other) noexcept;
    MemoryReservation & operator=(MemoryReservation && other) noexcept;
    MemoryReservation(const MemoryReservation &) = delete;
    MemoryReservation & operator=(const MemoryReservation &) = delete;

    bool acquired() const noexcept { return tracker_ != nullptr; }
    size_t bytes() const noexcept { return bytes_; }
    void reset() noexcept;

private:
    MemoryTracker * tracker_ = nullptr;
    size_t bytes_ = 0;
};

} // namespace popo::executor
