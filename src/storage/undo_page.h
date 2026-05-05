#pragma once
#include <cstring>
#include <optional>
#include <vector>
#include "src/storage/common.h"
#include "src/storage/page.h"

namespace neon::storage
{

// ============================================================================
// Undo Record Types
// ============================================================================

enum class UndoType : uint8_t
{
    INSERT = 0, // Rollback: DELETE the inserted row
    DELETE = 1, // Rollback: INSERT the deleted row back
    UPDATE = 2, // Rollback: restore old data
};

// ============================================================================
// Undo Record — stored in UndoPage
//   UndoRecordHeader (fixed size)
//   old_data[variable] (the before-image of the row)
// ============================================================================

struct UndoRecordHeader
{
    txn_id_t txn_id = INVALID_TXN_ID;
    UndoType type = UndoType::INSERT;
    page_id_t prev_undo_page = INVALID_PAGE_ID; // Link to previous undo record
    uint16_t prev_undo_offset = 0; // Offset of previous record in that page
    uint16_t old_data_len = 0; // Length of old_data following this header
    // Followed by: char old_data[old_data_len]
};

static constexpr uint16_t UNDO_RECORD_HEADER_SIZE = sizeof(UndoRecordHeader);

// ============================================================================
// UndoPage — 16KB page storing undo records (slotted-like structure)
// Layout:
//   PageHeader (PAGE_HEADER_SIZE)
//   record_count (uint16_t)
//   free_offset (uint16_t)     -- end of record index area (grows from front)
//   data_area_start (uint16_t) -- start of data area (grows from PAGE_SIZE downward)
//
//   UndoRecordIndex[record_count] growing from front:
//     uint16_t offset   -- offset of the record data within the page
//     uint16_t length   -- total length of record (header + old_data)
//
//   Record data growing from PAGE_SIZE downward:
//     Each: UndoRecordHeader + old_data[variable]
// ============================================================================

static constexpr uint16_t UNDO_INDEX_ENTRY_SIZE = sizeof(uint16_t) * 2; // offset + length

class UndoPage
{
public:
    void init(page_id_t pid)
    {
        std::memset(data_, 0, PAGE_SIZE);
        auto * hdr = header();
        hdr->page_id = pid;
        hdr->page_type = static_cast<uint16_t>(PageType::UNDO_PAGE);
        hdr->lsn = INVALID_LSN;
        hdr->slot_count = 0; // used as record_count
        hdr->free_space_offset = PAGE_HEADER_SIZE + sizeof(uint16_t) * 3; // hdr + count + free_off + data_start
        hdr->tuple_area_offset = PAGE_SIZE;
        set_record_count(0);
        set_data_area_start(PAGE_SIZE);
    }

    PageHeader * header() { return reinterpret_cast<PageHeader *>(data_); }
    const PageHeader * header() const { return reinterpret_cast<const PageHeader *>(data_); }

    uint16_t record_count() const { return *reinterpret_cast<const uint16_t *>(data_ + PAGE_HEADER_SIZE); }
    void set_record_count(uint16_t c) { *reinterpret_cast<uint16_t *>(data_ + PAGE_HEADER_SIZE) = c; }

    uint16_t data_area_start() const { return *reinterpret_cast<const uint16_t *>(data_ + PAGE_HEADER_SIZE + sizeof(uint16_t) * 2); }
    void set_data_area_start(uint16_t s) { *reinterpret_cast<uint16_t *>(data_ + PAGE_HEADER_SIZE + sizeof(uint16_t) * 2) = s; }

    // Free space between index area and data area
    uint16_t free_space() const
    {
        uint16_t idx_end = PAGE_HEADER_SIZE + sizeof(uint16_t) * 3 + record_count() * UNDO_INDEX_ENTRY_SIZE;
        uint16_t data_start = data_area_start();
        if (data_start <= idx_end)
            return 0;
        return data_start - idx_end;
    }

    // Append an undo record. Returns offset within page (where record starts).
    std::optional<uint16_t>
    append_undo(txn_id_t txn_id, UndoType type, const std::vector<char> & old_data, page_id_t prev_page, uint16_t prev_offset)
    {
        uint16_t total_len = UNDO_RECORD_HEADER_SIZE + static_cast<uint16_t>(old_data.size());
        uint16_t needed = UNDO_INDEX_ENTRY_SIZE + total_len;
        if (free_space() < needed)
        {
            return std::nullopt;
        }

        // Allocate data space from the end
        uint16_t new_data_start = data_area_start() - total_len;
        set_data_area_start(new_data_start);

        // Write UndoRecordHeader
        auto * rec_hdr = reinterpret_cast<UndoRecordHeader *>(data_ + new_data_start);
        rec_hdr->txn_id = txn_id;
        rec_hdr->type = type;
        rec_hdr->prev_undo_page = prev_page;
        rec_hdr->prev_undo_offset = prev_offset;
        rec_hdr->old_data_len = static_cast<uint16_t>(old_data.size());

        // Write old_data
        if (!old_data.empty())
        {
            std::memcpy(data_ + new_data_start + UNDO_RECORD_HEADER_SIZE, old_data.data(), old_data.size());
        }

        // Append index entry
        uint16_t idx = record_count();
        auto * idx_entry = reinterpret_cast<uint16_t *>(data_ + PAGE_HEADER_SIZE + sizeof(uint16_t) * 3 + idx * UNDO_INDEX_ENTRY_SIZE);
        idx_entry[0] = new_data_start; // offset
        idx_entry[1] = total_len; // length

        set_record_count(idx + 1);
        header()->free_space_offset += UNDO_INDEX_ENTRY_SIZE;

        return new_data_start;
    }

    // Read a record by its offset
    const UndoRecordHeader * get_record(uint16_t offset) const
    {
        if (offset >= data_area_start() && offset < PAGE_SIZE)
        {
            return reinterpret_cast<const UndoRecordHeader *>(data_ + offset);
        }
        return nullptr;
    }

    char * raw_data() { return data_; }
    const char * raw_data() const { return data_; }

private:
    char data_[PAGE_SIZE];
};

static_assert(sizeof(UndoPage) == PAGE_SIZE);

} // namespace neon::storage
