#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "executor/vector/vector_constants.h"

namespace popo::executor
{

// 64-byte aligned contiguous buffer (AVX-512 / AVX2 safe, padded for full vector width).
template <typename T>
class AlignedBuffer
{
public:
    AlignedBuffer() = default;
    explicit AlignedBuffer(size_t count) { resize(count); }

    ~AlignedBuffer() { release(); }

    AlignedBuffer(const AlignedBuffer &) = delete;
    AlignedBuffer & operator=(const AlignedBuffer &) = delete;

    AlignedBuffer(AlignedBuffer && other) noexcept
        : ptr_(other.ptr_), size_(other.size_), capacity_(other.capacity_)
    {
        other.ptr_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    AlignedBuffer & operator=(AlignedBuffer && other) noexcept
    {
        if (this != &other)
        {
            release();
            ptr_ = other.ptr_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.ptr_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    void resize(size_t count)
    {
        if (count > capacity_)
        {
            release();
            // Pad by at least 64 bytes so SIMD tail loads/stores never overrun.
            const size_t raw_bytes = count * sizeof(T) + kSimdAlignment;
            const size_t alloc_bytes = ((raw_bytes + kSimdAlignment - 1) / kSimdAlignment) * kSimdAlignment;
            void * mem = std::aligned_alloc(kSimdAlignment, alloc_bytes);
            if (mem != nullptr)
            {
                std::memset(mem, 0, alloc_bytes);
                ptr_ = static_cast<T *>(mem);
                capacity_ = count;
            }
        }
        size_ = count;
    }

    void clear_zero()
    {
        if (ptr_ && size_ > 0)
            std::memset(ptr_, 0, size_ * sizeof(T));
    }

    T * data() noexcept { return ptr_; }
    const T * data() const noexcept { return ptr_; }
    size_t size() const noexcept { return size_; }
    T & operator[](size_t i) noexcept { return ptr_[i]; }
    const T & operator[](size_t i) const noexcept { return ptr_[i]; }

private:
    void release() noexcept
    {
        if (ptr_ != nullptr)
        {
            std::free(ptr_);
            ptr_ = nullptr;
        }
        size_ = 0;
        capacity_ = 0;
    }

    T * ptr_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

} // namespace popo::executor
