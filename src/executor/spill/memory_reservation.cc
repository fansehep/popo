#include "executor/spill/memory_reservation.h"

#include <utility>

namespace popo::executor
{

MemoryReservation::MemoryReservation(MemoryTracker & tracker, size_t bytes)
{
    if (tracker.try_reserve(bytes))
    {
        tracker_ = &tracker;
        bytes_ = bytes;
    }
}

MemoryReservation::~MemoryReservation()
{
    reset();
}

MemoryReservation::MemoryReservation(MemoryReservation && other) noexcept
    : tracker_(std::exchange(other.tracker_, nullptr)), bytes_(std::exchange(other.bytes_, 0))
{
}

MemoryReservation & MemoryReservation::operator=(MemoryReservation && other) noexcept
{
    if (this != &other)
    {
        reset();
        tracker_ = std::exchange(other.tracker_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

void MemoryReservation::reset() noexcept
{
    if (tracker_)
        tracker_->release(bytes_);
    tracker_ = nullptr;
    bytes_ = 0;
}

} // namespace popo::executor
