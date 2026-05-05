#include "src/storage/undo_space.h"
#include <spdlog/spdlog.h>

namespace neon::storage
{

UndoSpace::UndoSpace(const std::string & table_name, BufferPoolManager * bpm, DiskManager * dm)
    : table_name_(table_name), bpm_(bpm), dm_(dm)
{
}

void UndoSpace::init()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!dm_->table_file_exists(table_name_))
    {
        dm_->create_table_file(table_name_);
    }

    page_id_t pc = dm_->page_count(table_name_);
    if (pc > 0)
    {
        current_page_id_ = pc; // last page
    }
    else
    {
        current_page_id_ = INVALID_PAGE_ID;
    }
    ensure_page();
}

void UndoSpace::ensure_page()
{
    if (current_page_id_ != INVALID_PAGE_ID)
    {
        // Check if current page has space
        auto guard = bpm_->fetch_page(table_name_, current_page_id_);
        if (guard.is_valid())
        {
            auto * page = reinterpret_cast<UndoPage *>(guard.data());
            // Need space for at least a small undo record
            if (page->free_space() >= UNDO_RECORD_HEADER_SIZE + UNDO_INDEX_ENTRY_SIZE + 64)
            {
                return;
            }
        }
    }

    // Allocate new page
    page_id_t new_pid = 0;
    auto guard = bpm_->new_page(table_name_, new_pid);
    if (!guard.is_valid())
    {
        spdlog::error("UndoSpace::ensure_page: failed to allocate new page");
        return;
    }

    auto * page = reinterpret_cast<UndoPage *>(guard.data());
    page->init(new_pid);
    guard.mark_dirty();

    current_page_id_ = new_pid;
}

UndoLoc UndoSpace::save_before_image(txn_id_t txn_id, UndoType type, const std::vector<char> & old_data, UndoLoc prev_undo)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_page();

    if (current_page_id_ == INVALID_PAGE_ID)
    {
        return UndoLoc{};
    }

    auto guard = bpm_->fetch_page(table_name_, current_page_id_);
    if (!guard.is_valid())
    {
        return UndoLoc{};
    }

    auto * page = reinterpret_cast<UndoPage *>(guard.data());
    auto offset_opt = page->append_undo(txn_id, type, old_data, prev_undo.page_id, prev_undo.offset);
    if (!offset_opt.has_value())
    {
        // Page might be full despite ensure_page check, retry with new page
        guard.mark_dirty();
        current_page_id_ = INVALID_PAGE_ID;
        ensure_page();

        auto guard2 = bpm_->fetch_page(table_name_, current_page_id_);
        if (!guard2.is_valid())
        {
            return UndoLoc{};
        }
        auto * page2 = reinterpret_cast<UndoPage *>(guard2.data());
        offset_opt = page2->append_undo(txn_id, type, old_data, prev_undo.page_id, prev_undo.offset);
        if (!offset_opt.has_value())
        {
            spdlog::error("UndoSpace::save_before_image: record too large for a single page");
            return UndoLoc{};
        }
        guard2.mark_dirty();
        return UndoLoc{current_page_id_, offset_opt.value()};
    }

    guard.mark_dirty();
    return UndoLoc{current_page_id_, offset_opt.value()};
}

std::optional<std::tuple<UndoType, std::vector<char>, UndoLoc>> UndoSpace::fetch_undo(UndoLoc loc) const
{
    if (!loc.is_valid())
    {
        return std::nullopt;
    }

    auto guard = bpm_->fetch_page(table_name_, loc.page_id);
    if (!guard.is_valid())
    {
        return std::nullopt;
    }

    const auto * page = reinterpret_cast<const UndoPage *>(guard.data());
    const auto * rec = page->get_record(loc.offset);
    if (!rec)
    {
        return std::nullopt;
    }

    std::vector<char> data(rec->old_data_len);
    if (rec->old_data_len > 0)
    {
        const char * src = reinterpret_cast<const char *>(rec) + UNDO_RECORD_HEADER_SIZE;
        std::memcpy(data.data(), src, rec->old_data_len);
    }

    UndoLoc prev{rec->prev_undo_page, rec->prev_undo_offset};
    return std::make_tuple(rec->type, std::move(data), prev);
}

void UndoSpace::rollback_txn(txn_id_t /*txn_id*/, UndoLoc first_undo, std::function<void(UndoType, const std::vector<char> &)> apply)
{
    UndoLoc current = first_undo;
    while (current.is_valid())
    {
        auto result = fetch_undo(current);
        if (!result.has_value())
        {
            break;
        }

        auto [type, old_data, prev] = result.value();
        apply(type, old_data);
        current = prev;
    }
}

} // namespace neon::storage
