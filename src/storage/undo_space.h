#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include "src/storage/buffer_pool.h"
#include "src/storage/common.h"
#include "src/storage/disk_manager.h"
#include "src/storage/page.h"
#include "src/storage/undo_page.h"

namespace neon::storage
{

// ============================================================================
// UndoLoc — location of an undo record (page + offset within page)
// ============================================================================

struct UndoLoc
{
    page_id_t page_id = INVALID_PAGE_ID;
    uint16_t offset = 0;

    bool is_valid() const { return page_id != INVALID_PAGE_ID; }
};

// ============================================================================
// UndoSpace — manages undo pages for transaction rollback
// Uses a dedicated file "__undo__" for undo storage.
// ============================================================================

class UndoSpace
{
public:
    UndoSpace(const std::string & table_name, BufferPoolManager * bpm, DiskManager * dm);

    // Initialize: ensure the undo file and first page exist
    void init();

    // Save a before-image. Returns location for undo chain linking.
    // prev_undo: previous undo record for the same transaction (for undo chain)
    UndoLoc save_before_image(txn_id_t txn_id, UndoType type, const std::vector<char> & old_data, UndoLoc prev_undo);

    // Fetch an undo record by location
    // Returns {type, old_data, prev_undo_loc}
    std::optional<std::tuple<UndoType, std::vector<char>, UndoLoc>> fetch_undo(UndoLoc loc) const;

    // Rollback a transaction: walk undo chain and apply callback for each record
    // apply_fn(type, old_data) — caller performs the actual undo operation
    void rollback_txn(txn_id_t txn_id, UndoLoc first_undo, std::function<void(UndoType, const std::vector<char> &)> apply);

    // Get access to buffer pool and disk manager
    BufferPoolManager * buffer_pool() { return bpm_; }

private:
    std::string table_name_; // e.g. "__undo__"
    BufferPoolManager * bpm_;
    DiskManager * dm_;
    mutable std::mutex mutex_;

    page_id_t current_page_id_ = INVALID_PAGE_ID;

    // Ensure a page with space exists for new undo records
    void ensure_page();
};

} // namespace neon::storage
