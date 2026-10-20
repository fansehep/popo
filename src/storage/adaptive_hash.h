#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "src/storage/common.h"

namespace popo::storage
{

// InnoDB-style adaptive hash: a leaf is promoted after a few searches, then
// every key on that page is hashed to its slot. The next point lookup skips
// the B+ tree. A write drops the hash and it is rebuilt from later searches.
class AdaptiveHashIndex
{
public:
    static constexpr uint32_t kHotAfter = 4;

    struct Loc
    {
        page_id_t page = INVALID_PAGE_ID;
        slot_id_t slot = 0;
    };

    std::optional<Loc> find(uint64_t fold) const
    {
        auto it = entries_.find(fold);
        if (it == entries_.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    // True only on the lookup that promotes the page.
    bool observe(page_id_t page)
    {
        uint32_t n = ++hits_[page];
        if (n == kHotAfter)
        {
            hot_.insert(page);
            return true;
        }
        return false;
    }

    bool hot(page_id_t page) const { return hot_.contains(page); }

    void remember(uint64_t fold, page_id_t page, slot_id_t slot)
    {
        entries_[fold] = Loc{page, slot};
        pages_[page].push_back(fold);
    }

    void clear()
    {
        entries_.clear();
        pages_.clear();
        hits_.clear();
        hot_.clear();
    }

private:
    std::unordered_map<uint64_t, Loc> entries_;
    std::unordered_map<page_id_t, std::vector<uint64_t>> pages_;
    std::unordered_map<page_id_t, uint32_t> hits_;
    std::unordered_set<page_id_t> hot_;
};

} // namespace popo::storage
