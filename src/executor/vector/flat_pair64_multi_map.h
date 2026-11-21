#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "executor/vector/hash_util.h"

namespace popo::executor
{

// Fast open-addressed (int64_t, int64_t) -> uint32_t multi-map (for 2-key joins like partsupp).
class FlatPair64MultiMap
{
public:
    static constexpr uint32_t kEmpty = UINT32_MAX;

    void build(
        std::span<const int64_t> k1, std::span<const int64_t> k2,
        std::span<const uint8_t> is_null)
    {
        const size_t n = k1.size();
        next_.assign(n, kEmpty);
        size_t cap = 16;
        while (cap < n * 2)
            cap <<= 1;
        keys1_.assign(cap, 0);
        keys2_.assign(cap, 0);
        heads_.assign(cap, kEmpty);
        const size_t mask = cap - 1;
        for (size_t i = 0; i < n; ++i)
        {
            if (!is_null.empty() && is_null[i])
                continue;
            const int64_t a = k1[i];
            const int64_t b = k2[i];
            size_t slot = static_cast<size_t>(hash_pair64_scalar(a, b)) & mask;
            while (heads_[slot] != kEmpty && (keys1_[slot] != a || keys2_[slot] != b))
                slot = (slot + 1) & mask;
            if (heads_[slot] == kEmpty)
            {
                keys1_[slot] = a;
                keys2_[slot] = b;
            }
            next_[i] = heads_[slot];
            heads_[slot] = static_cast<uint32_t>(i);
        }
    }

    uint32_t find_head(int64_t a, int64_t b) const noexcept
    {
        if (heads_.empty())
            return kEmpty;
        const size_t mask = heads_.size() - 1;
        size_t slot = static_cast<size_t>(hash_pair64_scalar(a, b)) & mask;
        while (heads_[slot] != kEmpty)
        {
            if (keys1_[slot] == a && keys2_[slot] == b)
                return heads_[slot];
            slot = (slot + 1) & mask;
        }
        return kEmpty;
    }

    uint32_t next_of(uint32_t idx) const noexcept { return next_[idx]; }

private:
    std::vector<int64_t> keys1_;
    std::vector<int64_t> keys2_;
    std::vector<uint32_t> heads_;
    std::vector<uint32_t> next_;
};

} // namespace popo::executor
