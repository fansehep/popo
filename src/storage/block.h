#pragma once

#include <cstdint>
#include "src/storage/common.h"

namespace popo::storage
{

// On-disk table file:
//   [FileHeader 4KB]
//   [Block 0: 1MB][Block 1: 1MB]...
// Each block is BlockMeta (fixed prefix) plus zstd-compressed pages appended
// after the meta. Payloads are never overwritten in place. A later write of the
// same logical page appends a new image; the previous image becomes garbage.
//
// Atomicity matches SQLite's single writer: one transaction, commit appends
// one record to commit.wal and fsyncs that file once. Table files are updated
// after the record is durable. Abort rewinds the block tail in memory. A crash
// before the commit record is durable keeps the previous committed generation.

constexpr uint32_t BLOCK_SIZE = 1u << 20;
constexpr uint32_t FILE_HEADER_SIZE = 4096;
constexpr uint32_t BLOCK_META_SIZE = 4096;
constexpr uint32_t MAX_PAGES_PER_BLOCK = 48;
constexpr uint32_t FILE_MAGIC = 0x4F504F50u; // 'POPO'
constexpr uint32_t BLOCK_MAGIC = 0x314B4C42u; // 'BLK1'
constexpr uint32_t FILE_VERSION = 1;
constexpr uint32_t NO_BLOCK = UINT32_MAX;
constexpr uint64_t GC_WASTE_THRESHOLD = 256ull * 1024ull;

enum BlockFlags : uint16_t
{
    BLOCK_OPEN = 0,
    BLOCK_SEALED = 1,
    BLOCK_FREE = 2,
};

enum PageEntryFlags : uint32_t
{
    ENTRY_DATA = 0,
    ENTRY_TOMBSTONE = 1,
};

struct PageEntry
{
    uint32_t page_id = 0;
    uint32_t offset = 0;
    uint32_t comp_size = 0;
    uint32_t flags = ENTRY_DATA;
    uint64_t gen = 0;
};

struct BlockMeta
{
    uint32_t magic = 0;
    uint32_t block_id = 0;
    uint16_t page_count = 0;
    uint16_t flags = BLOCK_FREE;
    uint32_t payload_end = BLOCK_META_SIZE;
    uint32_t reserved = 0;
    PageEntry entries[MAX_PAGES_PER_BLOCK]{};
};

struct FileHeader
{
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t block_size = 0;
    uint32_t block_count = 0;
    uint32_t next_page_id = 1;
    uint32_t open_block = NO_BLOCK;
    uint64_t committed_gen = 0;
};

static_assert(sizeof(PageEntry) == 24);
static_assert(sizeof(BlockMeta) <= BLOCK_META_SIZE);
static_assert(sizeof(FileHeader) <= FILE_HEADER_SIZE);
static_assert(alignof(PageEntry) == 8);

inline uint64_t block_file_offset(uint32_t block_id)
{
    return static_cast<uint64_t>(FILE_HEADER_SIZE) + static_cast<uint64_t>(block_id) * BLOCK_SIZE;
}

} // namespace popo::storage
