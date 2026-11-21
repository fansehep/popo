#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "executor/vector/hash_util.h"

namespace popo::executor
{

// Fast open-addressed int64 -> uint32_t multi-map using intrusive next[] array (for Hash Join).
class FlatInt64MultiMap
{
public:
    static constexpr uint32_t kEmpty = UINT32_MAX;

    void build(std::span<const int64_t> build_keys, std::span<const uint8_t> is_null)
    {
        const size_t n = build_keys.size();
        next_.assign(n, kEmpty);
        size_t cap = 16;
        while (cap < n * 2)
            cap <<= 1;
        keys_.assign(cap, 0);
        heads_.assign(cap, kEmpty);
        const size_t mask = cap - 1;
        for (size_t i = 0; i < n; ++i)
        {
            if (!is_null.empty() && is_null[i])
                continue;
            const int64_t k = build_keys[i];
            size_t slot = static_cast<size_t>(hash_i64_scalar(k)) & mask;
            while (heads_[slot] != kEmpty && keys_[slot] != k)
                slot = (slot + 1) & mask;
            if (heads_[slot] == kEmpty)
                keys_[slot] = k;
            next_[i] = heads_[slot];
            heads_[slot] = static_cast<uint32_t>(i);
        }
    }

    uint32_t find_head(int64_t key) const noexcept
    {
        if (heads_.empty())
            return kEmpty;
        const size_t mask = heads_.size() - 1;
        size_t slot = static_cast<size_t>(hash_i64_scalar(key)) & mask;
        while (heads_[slot] != kEmpty)
        {
            if (keys_[slot] == key)
                return heads_[slot];
            slot = (slot + 1) & mask;
        }
        return kEmpty;
    }

    uint32_t find_head_hashed(int64_t key, uint64_t hash) const noexcept
    {
        if (heads_.empty())
            return kEmpty;
        const size_t mask = heads_.size() - 1;
        size_t slot = static_cast<size_t>(hash) & mask;
        while (heads_[slot] != kEmpty)
        {
            if (keys_[slot] == key)
                return heads_[slot];
            slot = (slot + 1) & mask;
        }
        return kEmpty;
    }

    uint32_t next_of(uint32_t idx) const noexcept { return next_[idx]; }

private:
    std::vector<int64_t> keys_;
    std::vector<uint32_t> heads_;
    std::vector<uint32_t> next_;
};

} // namespace popo::executor
