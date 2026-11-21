#pragma once

#include <cstdint>

namespace popo::executor
{

inline uint64_t hash_i64_scalar(int64_t key) noexcept
{
    uint64_t h = static_cast<uint64_t>(key);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

inline uint64_t hash_pair64_scalar(int64_t k1, int64_t k2) noexcept
{
    const uint64_t h1 = hash_i64_scalar(k1);
    const uint64_t h2 = hash_i64_scalar(k2);
    return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
}

} // namespace popo::executor
