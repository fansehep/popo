#pragma once
#include <cstdint>
#include <functional>

namespace neon::storage
{

using page_id_t = uint32_t;
using frame_id_t = uint32_t;
using lsn_t = uint64_t;
using txn_id_t = uint64_t;
using slot_id_t = uint16_t;

constexpr uint32_t PAGE_SIZE = 16384; // 16KB
constexpr page_id_t INVALID_PAGE_ID = 0;
constexpr frame_id_t INVALID_FRAME_ID = UINT32_MAX;
constexpr lsn_t INVALID_LSN = 0;
constexpr txn_id_t INVALID_TXN_ID = 0;

struct RowId
{
    page_id_t page_id = INVALID_PAGE_ID;
    slot_id_t slot_id = 0;

    bool operator==(const RowId & other) const { return page_id == other.page_id && slot_id == other.slot_id; }
    bool operator!=(const RowId & other) const { return !(*this == other); }
    bool is_valid() const { return page_id != INVALID_PAGE_ID; }
};

} // namespace neon::storage

// Hash support for RowId
template <>
struct std::hash<neon::storage::RowId>
{
    size_t operator()(const neon::storage::RowId & rid) const
    {
        return std::hash<uint64_t>()((static_cast<uint64_t>(rid.page_id) << 16) | rid.slot_id);
    }
};
