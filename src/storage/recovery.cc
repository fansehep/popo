#include "src/storage/recovery.h"
#include "src/catalog/schema_io.h"
#include "src/storage/row_serializer.h"
#include "src/storage/storage_engine.h"
#include "src/storage/table_heap.h"
#include "src/trancation/mvcc.h"

namespace popo::storage
{

popo::base::Result<void> RecoveryManager::recover()
{
    auto records = log_->read_all_records();
    analysis(records);
    POPO_TRY(redo(records));
    // UNDO: no-steal means losers left no page effects; nothing to roll back.

    // Rebuild the committed version store from the recovered heap image.
    for (const auto & [name, schema] : engine_->catalog())
    {
        engine_->mvcc()->rebuild_from_heap(name);
    }
    return {};
}

void RecoveryManager::analysis(const std::vector<LogRecord> & records)
{
    winners_.clear();
    for (const auto & rec : records)
    {
        if (rec.type == LogType::COMMIT)
        {
            winners_.insert(rec.txn_id);
        }
    }
}

popo::base::Result<void> RecoveryManager::redo(const std::vector<LogRecord> & records)
{
    for (const auto & rec : records)
    {
        switch (rec.type)
        {
            case LogType::CREATE_TABLE: {
                auto schema = popo::catalog::deserialize_schema(rec.new_data.data(), static_cast<uint32_t>(rec.new_data.size()));
                if (!schema)
                {
                    return std::unexpected(schema.error());
                }
                POPO_TRY(engine_->recover_create_table(rec.table_name, *schema));
                break;
            }
            case LogType::DROP_TABLE:
                engine_->recover_drop_table(rec.table_name);
                break;
            case LogType::CREATE_INDEX: {
                auto schema = popo::catalog::deserialize_schema(rec.new_data.data(), static_cast<uint32_t>(rec.new_data.size()));
                if (!schema)
                {
                    return std::unexpected(schema.error());
                }
                for (const auto & idx : schema->indexes_)
                {
                    if (!idx.index_cols_.empty())
                    {
                        POPO_TRY(engine_->recover_create_index(rec.table_name, idx.index_name_, idx.index_cols_[0], idx.is_unique_));
                    }
                }
                break;
            }
            case LogType::INSERT:
            case LogType::UPDATE:
            case LogType::DELETE:
                if (winners_.count(rec.txn_id) != 0)
                {
                    apply_row(rec);
                }
                break;
            default:
                break;
        }
    }
    return {};
}

void RecoveryManager::apply_row(const LogRecord & rec)
{
    auto * heap = engine_->get_table_heap(rec.table_name);
    if (heap == nullptr)
    {
        return;
    }
    const auto & schema = heap->schema();
    auto * mvcc = engine_->mvcc();
    if (rec.type == LogType::DELETE)
    {
        if (rec.old_data.empty())
        {
            return;
        }
        auto row = RowSerializer::deserialize(rec.old_data.data(), static_cast<uint32_t>(rec.old_data.size()), schema);
        if (!schema.columns_.empty())
        {
            mvcc->remove_row(rec.table_name, schema.extract_pk(row));
        }
    }
    else
    {
        if (rec.new_data.empty())
        {
            return;
        }
        auto row = RowSerializer::deserialize(rec.new_data.data(), static_cast<uint32_t>(rec.new_data.size()), schema);
        mvcc->install_row(rec.table_name, row);
    }
}

} // namespace popo::storage
