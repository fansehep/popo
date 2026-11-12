#pragma once
#include <unordered_set>
#include "src/base/error.h"
#include "src/storage/common.h"
#include "src/storage/redo_log.h"

namespace popo::storage
{

class StorageEngine;

// ARIES-style crash recovery over the logical write-ahead log.
class RecoveryManager
{
public:
    RecoveryManager(StorageEngine * engine, RedoLogManager * log) : engine_(engine), log_(log) { }

    popo::base::Result<void> recover();

private:
    StorageEngine * engine_;
    RedoLogManager * log_;
    std::unordered_set<txn_id_t> winners_;

    void analysis(const std::vector<LogRecord> & records);
    popo::base::Result<void> redo(const std::vector<LogRecord> & records);
    void apply_row(const LogRecord & rec);
};

} // namespace popo::storage
