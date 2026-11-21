#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "executor/vector/hash_util.h"

namespace popo::executor
{

// Fast open-addressed int64 hash set (zero per-node mallocs).
class FlatInt64Set
{
public:
    void reserve(size_t expected)
    {
        size_t cap = 16;
        while (cap < expected * 2)
            cap <<= 1;
        if (cap > keys_.size())
            rehash(cap);
    }

    void insert(int64_t key)
    {
        if (keys_.empty() || (size_ + 1) * 10 >= keys_.size() * 7)
            rehash(keys_.empty() ? 64 : keys_.size() * 2);
        const size_t mask = keys_.size() - 1;
        size_t slot = static_cast<size_t>(hash_i64_scalar(key)) & mask;
        while (occupied_[slot])
        {
            if (keys_[slot] == key)
                return;
            slot = (slot + 1) & mask;
        }
        occupied_[slot] = 1;
        keys_[slot] = key;
        ++size_;
    }

    bool contains(int64_t key) const noexcept
    {
        if (keys_.empty())
            return false;
        const size_t mask = keys_.size() - 1;
        size_t slot = static_cast<size_t>(hash_i64_scalar(key)) & mask;
        while (occupied_[slot])
        {
            if (keys_[slot] == key)
                return true;
            slot = (slot + 1) & mask;
        }
        return false;
    }

    bool empty() const noexcept { return size_ == 0; }
    size_t size() const noexcept { return size_; }

private:
    void rehash(size_t new_cap)
    {
        std::vector<int64_t> old_keys = std::move(keys_);
        std::vector<uint8_t> old_occ = std::move(occupied_);
        keys_.assign(new_cap, 0);
        occupied_.assign(new_cap, 0);
        size_ = 0;
        const size_t mask = new_cap - 1;
        for (size_t i = 0; i < old_keys.size(); ++i)
        {
            if (!old_occ[i])
                continue;
            size_t slot = static_cast<size_t>(hash_i64_scalar(old_keys[i])) & mask;
            while (occupied_[slot])
                slot = (slot + 1) & mask;
            occupied_[slot] = 1;
            keys_[slot] = old_keys[i];
            ++size_;
        }
    }

    std::vector<int64_t> keys_;
    std::vector<uint8_t> occupied_;
    size_t size_ = 0;
};

} // namespace popo::executor
