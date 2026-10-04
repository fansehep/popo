#pragma once
#include <cstring>
#include <functional>
#include <vector>
#include "src/storage/common.h"
#include "src/storage/page.h"

namespace neon::storage
{

// ============================================================================
// B+Tree Internal Page Layout:
//   PageHeader (PAGE_HEADER_SIZE bytes)
//   level (uint16_t)           -- tree level (0 = just above leaf)
//   key_count (uint16_t)       -- number of keys (children = key_count + 1)
//   InternalEntry[key_count]   -- grows from front (after header)
//   ... free space ...
//   key data area              -- grows from end (PAGE_SIZE downward)
//   rightmost_child (page_id_t) -- stored right after last entry
//
// Each InternalEntry: {child(page_id_t, 4B), key_offset(uint16_t, 2B), key_len(uint16_t, 2B)} = 8 bytes
// ============================================================================

struct InternalEntry
{
    page_id_t child; // Page ID of child subtree (for keys < this entry's key)
    uint16_t key_offset; // Offset within page where key data starts
    uint16_t key_len; // Length of key data
};

static constexpr uint16_t INTERNAL_ENTRY_SIZE = sizeof(InternalEntry);

class BTreeInternalPage
{
public:
    static constexpr uint16_t INTERNAL_HEADER_EXTRA = sizeof(uint16_t) * 2; // level + key_count
    static constexpr uint16_t HEADER_TOTAL = PAGE_HEADER_SIZE + INTERNAL_HEADER_EXTRA;

    // Initialize as a new internal node
    void init(page_id_t pid, uint16_t level)
    {
        std::memset(data_, 0, PAGE_SIZE);
        auto * hdr = header();
        hdr->page_id = pid;
        hdr->page_type = static_cast<uint16_t>(PageType::BPTREE_INTERNAL_PAGE);
        hdr->lsn = INVALID_LSN;
        hdr->slot_count = 0; // not used for internal pages
        hdr->free_space_offset = HEADER_TOTAL;
        hdr->tuple_area_offset = PAGE_SIZE;
        hdr->next_page_id = INVALID_PAGE_ID;
        set_level(level);
        set_key_count(0);
    }

    void init_as_root() { init(INVALID_PAGE_ID, 0); }

    PageHeader * header() { return reinterpret_cast<PageHeader *>(data_); }
    const PageHeader * header() const { return reinterpret_cast<const PageHeader *>(data_); }

    uint16_t level() const { return *reinterpret_cast<const uint16_t *>(data_ + PAGE_HEADER_SIZE); }
    void set_level(uint16_t lvl) { *reinterpret_cast<uint16_t *>(data_ + PAGE_HEADER_SIZE) = lvl; }

    uint16_t key_count() const { return *reinterpret_cast<const uint16_t *>(data_ + PAGE_HEADER_SIZE + sizeof(uint16_t)); }
    void set_key_count(uint16_t count) { *reinterpret_cast<uint16_t *>(data_ + PAGE_HEADER_SIZE + sizeof(uint16_t)) = count; }

    // Access the i-th entry (0-indexed)
    InternalEntry * entry_at(int i) { return reinterpret_cast<InternalEntry *>(data_ + HEADER_TOTAL + i * INTERNAL_ENTRY_SIZE); }
    const InternalEntry * entry_at(int i) const
    {
        return reinterpret_cast<const InternalEntry *>(data_ + HEADER_TOTAL + i * INTERNAL_ENTRY_SIZE);
    }

    // Rightmost child is stored right AFTER all entries
    page_id_t rightmost_child() const
    {
        uint16_t kc = key_count();
        return *reinterpret_cast<const page_id_t *>(data_ + HEADER_TOTAL + kc * INTERNAL_ENTRY_SIZE);
    }
    void set_rightmost_child(page_id_t pid)
    {
        uint16_t kc = key_count();
        *reinterpret_cast<page_id_t *>(data_ + HEADER_TOTAL + kc * INTERNAL_ENTRY_SIZE) = pid;
    }

    // Get child at index i (0..key_count, where key_count is rightmost)
    page_id_t child_at(int i) const
    {
        if (i < static_cast<int>(key_count()))
        {
            return entry_at(i)->child;
        }
        return rightmost_child();
    }

    // Get key data of i-th entry
    const char * key_data_at(int i) const
    {
        const auto * entry = entry_at(i);
        return data_ + entry->key_offset;
    }

    // Free space: space between entries area and key data area
    uint16_t free_space() const
    {
        uint16_t kc = key_count();
        uint16_t entry_end = HEADER_TOTAL + kc * INTERNAL_ENTRY_SIZE + sizeof(page_id_t); // + rightmost child
        uint16_t data_start = header()->tuple_area_offset;
        if (data_start <= entry_end)
            return 0;
        return data_start - entry_end;
    }

    // Compare two keys using the provided comparator
    // Returns <0 if a < b, 0 if a == b, >0 if a > b
    using KeyComparator = std::function<int(const char * a_data, uint16_t a_len, const char * b_data, uint16_t b_len)>;

    // Find the child page to follow for a given search key
    page_id_t find_child(const char * key, uint16_t key_len, const KeyComparator & cmp) const
    {
        uint16_t kc = key_count();
        // Binary search: find first entry where key < entry_key (or equal)
        int lo = 0, hi = kc;
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;
            const auto * entry = entry_at(mid);
            int c = cmp(key, key_len, data_ + entry->key_offset, entry->key_len);
            if (c < 0)
            {
                hi = mid;
            }
            else
            {
                lo = mid + 1;
            }
        }
        return child_at(lo);
    }

    // Insert a (key, child_page) pair. child_page is the RIGHT child of this key.
    // Returns true if inserted, false if page is full.
    bool insert(const char * key, uint16_t key_len, page_id_t child_page, const KeyComparator & cmp)
    {
        uint16_t kc = key_count();
        uint16_t needed = INTERNAL_ENTRY_SIZE + key_len;
        if (free_space() < needed)
        {
            return false;
        }

        // Find insertion position
        int pos = 0;
        while (pos < kc && cmp(key, key_len, key_data_at(pos), entry_at(pos)->key_len) >= 0)
        {
            pos++;
        }

        // If pos == kc, we're inserting after all existing keys
        // The rightmost child becomes the child of the new entry,
        // and the new child_page becomes the new rightmost child

        if (pos == kc)
        {
            // New entry at the end: rightmost_child becomes entry.child, new child becomes rightmost
            page_id_t old_rightmost = rightmost_child();

            // Allocate key data space from the end
            header()->tuple_area_offset -= key_len;
            std::memcpy(data_ + header()->tuple_area_offset, key, key_len);

            // Shift existing entries? No, we just add at the end
            InternalEntry * entry = entry_at(pos);
            entry->child = old_rightmost;
            entry->key_offset = header()->tuple_area_offset;
            entry->key_len = key_len;

            set_key_count(kc + 1);

            // Set new rightmost child, move it to after the new entry
            *reinterpret_cast<page_id_t *>(data_ + HEADER_TOTAL + (kc + 1) * INTERNAL_ENTRY_SIZE) = child_page;
        }
        else
        {
            // Insert in the middle: need to shift entries from pos to kc-1 right by one.
            // entry[pos] keeps its old child (keys < new_key).
            // entry[pos+1] gets child_page (keys >= new_key but < old key at pos+1).
            // Save old child at pos and rightmost BEFORE shifting.
            page_id_t old_rightmost = rightmost_child();
            page_id_t old_child_at_pos = entry_at(pos)->child;

            // Shift entries (including the rightmost child slot)
            for (int i = kc; i > pos; i--)
            {
                std::memcpy(entry_at(i), entry_at(i - 1), INTERNAL_ENTRY_SIZE);
            }

            // Move the saved rightmost child to its new position
            *reinterpret_cast<page_id_t *>(data_ + HEADER_TOTAL + (kc + 1) * INTERNAL_ENTRY_SIZE) = old_rightmost;

            // Allocate key data space
            header()->tuple_area_offset -= key_len;
            std::memcpy(data_ + header()->tuple_area_offset, key, key_len);

            // entry[pos]: same old child (keys < new_key), new key
            InternalEntry * entry = entry_at(pos);
            entry->child = old_child_at_pos;
            entry->key_offset = header()->tuple_area_offset;
            entry->key_len = key_len;

            // entry[pos+1] (shifted from old pos): child_page is its new child (keys >= new_key)
            entry_at(pos + 1)->child = child_page;

            set_key_count(kc + 1);
        }

        return true;
    }

    // Split this page: move half the entries to new_page.
    // Returns {promoted_key, new_page_id}
    std::pair<std::vector<char>, page_id_t> split(BTreeInternalPage * new_page)
    {
        uint16_t kc = key_count();
        uint16_t mid = kc / 2;

        // Set the same level on the new page
        new_page->init(INVALID_PAGE_ID, level());

        // Copy entries [mid+1 .. kc-1] to new_page
        for (uint16_t i = mid + 1; i < kc; i++)
        {
            const auto * src_entry = entry_at(i);
            new_page->insert(
                data_ + src_entry->key_offset,
                src_entry->key_len,
                src_entry->child,
                [](const char *, uint16_t, const char *, uint16_t) { return 0; });
        }
        // Set rightmost child of new page
        new_page->set_rightmost_child(rightmost_child());

        // The middle key is promoted upward
        const auto * mid_entry = entry_at(mid);
        std::vector<char> promoted_key(data_ + mid_entry->key_offset, data_ + mid_entry->key_offset + mid_entry->key_len);
        page_id_t new_page_id = INVALID_PAGE_ID; // will be set by caller

        // Keep entries [0..mid-1] in this page, mid_entry.child becomes rightmost child
        set_rightmost_child(mid_entry->child);
        set_key_count(mid);

        return {promoted_key, new_page_id};
    }

    // Point a child pointer from old_pid to new_pid. Returns false if old_pid is absent.
    bool replace_child(page_id_t old_pid, page_id_t new_pid)
    {
        uint16_t kc = key_count();
        for (uint16_t i = 0; i < kc; ++i)
        {
            if (entry_at(i)->child == old_pid)
            {
                entry_at(i)->child = new_pid;
                return true;
            }
        }
        if (rightmost_child() == old_pid)
        {
            set_rightmost_child(new_pid);
            return true;
        }
        return false;
    }

    // Write page_id into header (used after allocating a page for split)
    void set_page_id(page_id_t pid) { header()->page_id = pid; }

    char * raw_data() { return data_; }
    const char * raw_data() const { return data_; }

private:
    char data_[PAGE_SIZE];
};

static_assert(sizeof(BTreeInternalPage) == PAGE_SIZE);

} // namespace neon::storage
