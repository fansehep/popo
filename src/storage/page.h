#pragma once
#include <cstring>
#include <functional>
#include <optional>
#include <vector>
#include "src/storage/common.h"

namespace neon::storage
{

enum class PageType : uint16_t
{
    INVALID = 0,
    DATA_PAGE = 1,
    META_PAGE = 2,
    BPTREE_INTERNAL_PAGE = 3, // B+Tree internal node
    BPTREE_LEAF_PAGE = 4, // B+Tree leaf node (reuses DataPage slotted structure)
    UNDO_PAGE = 5, // Undo record page
};

// Packed page header at beginning of each page
struct PageHeader
{
    page_id_t page_id = INVALID_PAGE_ID;
    uint16_t page_type = static_cast<uint16_t>(PageType::INVALID);
    lsn_t lsn = INVALID_LSN;
    uint16_t slot_count = 0;
    uint16_t free_space_offset = 0; // End of slot directory (start of free space)
    uint16_t tuple_area_offset = 0; // Start of tuple data area (grows from PAGE_SIZE downward)
    uint32_t next_page_id = INVALID_PAGE_ID; // For linked page list
};

static constexpr uint16_t PAGE_HEADER_SIZE = sizeof(PageHeader);

enum SlotFlags : uint8_t
{
    SLOT_VALID = 0,
    SLOT_DELETED = 1,
};

struct SlotEntry
{
    uint16_t offset = 0; // Offset of tuple data within page
    uint16_t length = 0; // Length of tuple data
    uint8_t flags = SLOT_VALID;
};

static constexpr uint16_t SLOT_ENTRY_SIZE = sizeof(SlotEntry);

// DataPage wraps a raw char[PAGE_SIZE] buffer and provides slotted page operations
class DataPage
{
public:
    // Initialize a NEW page (format it)
    void init(page_id_t pid)
    {
        std::memset(data_, 0, PAGE_SIZE);
        auto * hdr = header();
        hdr->page_id = pid;
        hdr->page_type = static_cast<uint16_t>(PageType::DATA_PAGE);
        hdr->lsn = INVALID_LSN;
        hdr->slot_count = 0;
        hdr->free_space_offset = PAGE_HEADER_SIZE;
        hdr->tuple_area_offset = PAGE_SIZE;
        hdr->next_page_id = INVALID_PAGE_ID;
    }

    // Get header
    PageHeader * header() { return reinterpret_cast<PageHeader *>(data_); }
    const PageHeader * header() const { return reinterpret_cast<const PageHeader *>(data_); }

    // Insert tuple data, returns slot_id. Returns nullopt if no space.
    std::optional<slot_id_t> insert_tuple(const char * data, uint16_t len)
    {
        auto * hdr = header();
        // Need space for one SlotEntry + tuple data
        uint16_t required = SLOT_ENTRY_SIZE + len;
        if (free_space() < required)
        {
            return std::nullopt;
        }

        // Allocate tuple space from the end
        hdr->tuple_area_offset -= len;
        std::memcpy(data_ + hdr->tuple_area_offset, data, len);

        // Allocate slot entry at the end of the slot directory
        slot_id_t slot_id = hdr->slot_count;
        auto * slot = slot_at(slot_id);
        slot->offset = hdr->tuple_area_offset;
        slot->length = len;
        slot->flags = SLOT_VALID;

        hdr->slot_count++;
        hdr->free_space_offset += SLOT_ENTRY_SIZE;

        return slot_id;
    }

    // Delete tuple at slot_id (mark as deleted)
    bool delete_tuple(slot_id_t slot_id)
    {
        auto * hdr = header();
        if (slot_id >= hdr->slot_count)
        {
            return false;
        }
        auto * slot = slot_at(slot_id);
        if (slot->flags == SLOT_DELETED)
        {
            return false;
        }
        slot->flags = SLOT_DELETED;
        return true;
    }

    // Get tuple data at slot_id. Returns {pointer, length}, or {nullptr, 0} if deleted/invalid.
    std::pair<const char *, uint16_t> get_tuple(slot_id_t slot_id) const
    {
        auto * hdr = header();
        if (slot_id >= hdr->slot_count)
        {
            return {nullptr, 0};
        }
        const auto * slot = slot_at(slot_id);
        if (slot->flags == SLOT_DELETED)
        {
            return {nullptr, 0};
        }
        return {data_ + slot->offset, slot->length};
    }

    // Update tuple at slot_id. If new data fits in old slot, update in-place.
    // If not, returns nullopt (caller should delete + re-insert to maintain sort order).
    std::optional<slot_id_t> update_tuple(slot_id_t slot_id, const char * data, uint16_t len)
    {
        auto * hdr = header();
        if (slot_id >= hdr->slot_count)
        {
            return std::nullopt;
        }
        auto * slot = slot_at(slot_id);
        if (slot->flags == SLOT_DELETED)
        {
            return std::nullopt;
        }

        // Only update in-place if data fits in existing slot
        if (len <= slot->length)
        {
            std::memcpy(data_ + slot->offset, data, len);
            slot->length = len;
            return slot_id;
        }

        // Data too large for in-place update — return nullopt.
        // Caller will handle via delete + sorted re-insert to maintain B+Tree order.
        return std::nullopt;
    }

    // Get free space available
    uint16_t free_space() const
    {
        auto * hdr = header();
        if (hdr->tuple_area_offset <= hdr->free_space_offset)
        {
            return 0;
        }
        return hdr->tuple_area_offset - hdr->free_space_offset;
    }

    // Get slot count
    uint16_t slot_count() const { return header()->slot_count; }

    // --- B+Tree leaf page support ---

    // Initialize as a B+Tree leaf page
    void init_as_leaf(page_id_t pid)
    {
        std::memset(data_, 0, PAGE_SIZE);
        auto * hdr = header();
        hdr->page_id = pid;
        hdr->page_type = static_cast<uint16_t>(PageType::BPTREE_LEAF_PAGE);
        hdr->lsn = INVALID_LSN;
        hdr->slot_count = 0;
        hdr->free_space_offset = PAGE_HEADER_SIZE;
        hdr->tuple_area_offset = PAGE_SIZE;
        hdr->next_page_id = INVALID_PAGE_ID;
    }

    // Insert tuple in sorted order. comparator(a_data, a_len, b_data, b_len) returns <0 if a<b.
    // Returns slot_id or nullopt if no space.
    using TupleComparator = std::function<int(const char * a_data, uint16_t a_len, const char * b_data, uint16_t b_len)>;
    std::optional<slot_id_t> insert_tuple_sorted(const char * data, uint16_t len, const TupleComparator & cmp)
    {
        auto * hdr = header();
        uint16_t required = SLOT_ENTRY_SIZE + len;
        if (free_space() < required)
        {
            return std::nullopt;
        }

        // Find insertion position: first slot where data < existing tuple
        uint16_t pos = 0;
        while (pos < hdr->slot_count)
        {
            const auto * slot = slot_at(pos);
            if (slot->flags == SLOT_DELETED)
            {
                pos++;
                continue;
            }
            int c = cmp(data, len, data_ + slot->offset, slot->length);
            if (c < 0)
                break;
            pos++;
        }

        // Shift slots [pos..slot_count-1] down by one
        uint16_t slot_dir_start = PAGE_HEADER_SIZE;
        uint16_t slot_dir_end = hdr->free_space_offset;
        uint16_t shift_start = slot_dir_start + pos * SLOT_ENTRY_SIZE;
        uint16_t shift_len = slot_dir_end - shift_start;
        if (shift_len > 0)
        {
            std::memmove(data_ + shift_start + SLOT_ENTRY_SIZE, data_ + shift_start, shift_len);
        }
        hdr->free_space_offset += SLOT_ENTRY_SIZE;

        // Allocate tuple space from the end
        hdr->tuple_area_offset -= len;
        std::memcpy(data_ + hdr->tuple_area_offset, data, len);

        // Set the new slot at pos
        auto * slot = slot_at(pos);
        slot->offset = hdr->tuple_area_offset;
        slot->length = len;
        slot->flags = SLOT_VALID;

        hdr->slot_count++;
        return static_cast<slot_id_t>(pos);
    }

    // Split this leaf page: move the latter half of slots to new_page.
    // Entries are moved as raw data. Key data is part of the serialized tuple.
    // Returns the number of entries moved.
    uint16_t split_leaf_entries(DataPage * new_page)
    {
        uint16_t total = slot_count();
        uint16_t mid = total / 2;
        uint16_t moved = 0;

        for (uint16_t i = mid; i < total; i++)
        {
            auto * slot = slot_at(i);
            if (slot->flags == SLOT_DELETED)
                continue;
            // Insert into new page (simple append since they're already sorted)
            new_page->insert_tuple(data_ + slot->offset, slot->length);
            slot->flags = SLOT_DELETED;
            moved++;
        }

        return moved;
    }

    // Get next leaf page ID (sibling link for range scan)
    page_id_t next_leaf_page() const { return header()->next_page_id; }

    // Set next leaf page ID
    void set_next_leaf_page(page_id_t pid) { header()->next_page_id = pid; }

    // Raw data access
    char * data() { return data_; }
    const char * data() const { return data_; }

    // Compact: remove DELETED slots AND repack data contiguously.
    //
    // Problem: insert_tuple_sorted allocates data at tuple_area_offset (insertion
    // order), but slots are sorted by PK. After split_leaf_entries + multiple
    // inserts/deletes, the data area becomes fragmented with dead space gaps
    // between valid entries. This causes free_space() to over-report available
    // space, leading to data corruption on subsequent inserts.
    //
    // Solution: two-phase compaction:
    //   1. Compact slot directory (remove DELETED slots)
    //   2. Repack all valid data contiguously from PAGE_SIZE downward,
    //      in reverse slot order, so slot[0] data is at the highest offset.
    // Returns number of slots removed.
    uint16_t compact()
    {
        auto * hdr = header();
        uint16_t old_count = hdr->slot_count;

        // Phase 1: Compact slot directory — move valid slots to front
        uint16_t write_pos = 0;
        for (uint16_t read_pos = 0; read_pos < old_count; read_pos++)
        {
            const auto * slot = slot_at(read_pos);
            if (slot->flags == SLOT_VALID)
            {
                if (write_pos != read_pos)
                {
                    *slot_at(write_pos) = *slot;
                }
                write_pos++;
            }
        }

        if (write_pos == old_count)
            return 0; // Nothing to compact

        // Phase 2: Repack data contiguously from PAGE_SIZE downward.
        // Use a temporary buffer to avoid memmove overlapping issues.
        // Because slots are in PK-sorted order but data was allocated in
        // insertion order, the old offsets can interleave with new ones.
        uint16_t total_data = 0;
        for (uint16_t i = 0; i < write_pos; i++)
        {
            total_data += slot_at(i)->length;
        }

        if (total_data > 0)
        {
            // Copy all valid data into temp buffer in slot order
            std::vector<char> temp(total_data);
            uint16_t temp_off = 0;
            for (uint16_t i = 0; i < write_pos; i++)
            {
                auto * slot = slot_at(i);
                std::memcpy(temp.data() + temp_off, data_ + slot->offset, slot->length);
                temp_off += slot->length;
            }

            // Copy back contiguously from PAGE_SIZE - total_data
            uint16_t data_start = PAGE_SIZE - total_data;
            std::memcpy(data_ + data_start, temp.data(), total_data);

            // Update slot offsets
            uint16_t data_end = data_start;
            for (uint16_t i = 0; i < write_pos; i++)
            {
                auto * slot = slot_at(i);
                slot->offset = data_end;
                data_end += slot->length;
            }

            hdr->tuple_area_offset = data_start;
        }
        else
        {
            hdr->tuple_area_offset = PAGE_SIZE;
        }

        hdr->slot_count = write_pos;
        hdr->free_space_offset = PAGE_HEADER_SIZE + write_pos * SLOT_ENTRY_SIZE;

        return old_count - write_pos;
    }

private:
    char data_[PAGE_SIZE];

    SlotEntry * slot_at(slot_id_t idx) { return reinterpret_cast<SlotEntry *>(data_ + PAGE_HEADER_SIZE + idx * SLOT_ENTRY_SIZE); }
    const SlotEntry * slot_at(slot_id_t idx) const
    {
        return reinterpret_cast<const SlotEntry *>(data_ + PAGE_HEADER_SIZE + idx * SLOT_ENTRY_SIZE);
    }
};

static_assert(sizeof(DataPage) == PAGE_SIZE);

} // namespace neon::storage
