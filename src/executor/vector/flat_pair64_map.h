#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "executor/vector/hash_util.h"

namespace popo::executor
{

// Fast open-addressed (int64_t, int64_t) -> V map.
template <typename V>
class FlatPair64Map
{
public:
    void reserve(size_t expected)
    {
        size_t cap = 16;
        while (cap < expected * 2)
            cap <<= 1;
        if (cap > keys1_.size())
            rehash(cap);
    }

    V & get_or_insert(int64_t a, int64_t b, bool & inserted)
    {
        if (keys1_.empty() || (size_ + 1) * 10 >= keys1_.size() * 7)
            rehash(keys1_.empty() ? 64 : keys1_.size() * 2);
        const size_t mask = keys1_.size() - 1;
        size_t slot = static_cast<size_t>(hash_pair64_scalar(a, b)) & mask;
        while (occupied_[slot])
        {
            if (keys1_[slot] == a && keys2_[slot] == b)
            {
                inserted = false;
                return values_[slot];
            }
            slot = (slot + 1) & mask;
        }
        occupied_[slot] = 1;
        keys1_[slot] = a;
        keys2_[slot] = b;
        values_[slot] = V{};
        ++size_;
        inserted = true;
        return values_[slot];
    }

    const V * find(int64_t a, int64_t b) const noexcept
    {
        if (keys1_.empty())
            return nullptr;
        const size_t mask = keys1_.size() - 1;
        size_t slot = static_cast<size_t>(hash_pair64_scalar(a, b)) & mask;
        while (occupied_[slot])
        {
            if (keys1_[slot] == a && keys2_[slot] == b)
                return &values_[slot];
            slot = (slot + 1) & mask;
        }
        return nullptr;
    }

    size_t size() const noexcept { return size_; }

    template <typename Fn>
    void for_each(Fn && fn)
    {
        for (size_t i = 0; i < keys1_.size(); ++i)
        {
            if (occupied_[i])
                fn(keys1_[i], keys2_[i], values_[i]);
        }
    }

private:
    void rehash(size_t new_cap)
    {
        std::vector<int64_t> ok1 = std::move(keys1_);
        std::vector<int64_t> ok2 = std::move(keys2_);
        std::vector<V> ov = std::move(values_);
        std::vector<uint8_t> oo = std::move(occupied_);
        keys1_.assign(new_cap, 0);
        keys2_.assign(new_cap, 0);
        values_.resize(new_cap);
        occupied_.assign(new_cap, 0);
        size_ = 0;
        const size_t mask = new_cap - 1;
        for (size_t i = 0; i < ok1.size(); ++i)
        {
            if (!oo[i])
                continue;
            size_t slot = static_cast<size_t>(hash_pair64_scalar(ok1[i], ok2[i])) & mask;
            while (occupied_[slot])
                slot = (slot + 1) & mask;
            occupied_[slot] = 1;
            keys1_[slot] = ok1[i];
            keys2_[slot] = ok2[i];
            values_[slot] = std::move(ov[i]);
            ++size_;
        }
    }

    std::vector<int64_t> keys1_;
    std::vector<int64_t> keys2_;
    std::vector<V> values_;
    std::vector<uint8_t> occupied_;
    size_t size_ = 0;
};

} // namespace popo::executor
