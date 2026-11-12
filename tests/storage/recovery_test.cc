#include <filesystem>
#include <random>
#include <string>
#include <gtest/gtest.h>
#include "src/catalog/column.h"
#include "src/catalog/schema.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/storage_engine.h"
#include "src/trancation/mvcc.h"
#include "src/trancation/txn.h"
#include "src/type/value.h"

namespace
{

using namespace popo::storage;
using namespace popo::type;
using namespace popo::catalog;

Schema make_schema(const std::string & name)
{
    Schema schema;
    schema.id_ = 1;
    schema.table_name_ = name;
    schema.primary_key_col_idx_ = 0;

    Column id;
    id.column_name_ = "id";
    id.data_type_ = ValueType::Int32;
    id.is_primary_key_ = true;
    id.is_nullable_ = false;
    id.type_len_ = 4;
    schema.columns_.push_back(id);

    Column v;
    v.column_name_ = "v";
    v.data_type_ = ValueType::Int32;
    v.is_primary_key_ = false;
    v.is_nullable_ = false;
    v.type_len_ = 4;
    schema.columns_.push_back(v);

    return schema;
}

Row make_row(int32_t id, int32_t v)
{
    Row row;
    row.append_value(Value(ValueType::Int32, id));
    row.append_value(Value(ValueType::Int32, v));
    return row;
}

class RecoveryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::random_device rd;
        dir_ = std::filesystem::temp_directory_path() / ("popo_recovery_" + std::to_string(rd()));
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    // A crash loses every dirty frame and runs no shutdown hook. The engine is
    // leaked deliberately: deleting it would checkpoint and mask the replay.
    popo::base::Result<StorageEngine *> crash_after_writes(bool commit)
    {
        auto * engine = new StorageEngine(dir_.string(), 64);
        auto initialized = engine->init();
        if (!initialized)
        {
            return std::unexpected(initialized.error());
        }
        auto created = engine->create_table("t", make_schema("t"));
        if (!created)
        {
            return std::unexpected(created.error());
        }
        auto * txn = engine->begin_txn();
        for (int32_t i = 1; i <= 50; ++i)
        {
            auto inserted = engine->mvcc()->stage_insert(txn, "t", make_row(i, i * 10));
            if (!inserted)
            {
                return std::unexpected(inserted.error());
            }
        }
        if (commit)
        {
            auto committed = engine->commit_txn(txn);
            if (!committed)
            {
                return std::unexpected(committed.error());
            }
        }
        engine->buffer_pool()->invalidate_all();
        return engine;
    }

    std::filesystem::path dir_;
};

TEST_F(RecoveryTest, CommittedRowsSurviveLosingEveryDirtyPage)
{
    auto crashed = crash_after_writes(true);
    ASSERT_TRUE(crashed);
    ASSERT_GE((*crashed)->redo_log()->flushed_lsn(), 1u);

    StorageEngine recovered(dir_.string(), 64);
    ASSERT_TRUE(recovered.init());

    auto rows = recovered.mvcc()->current_rows("t");
    ASSERT_EQ(rows.size(), 50u);
    for (size_t i = 0; i < rows.size(); ++i)
    {
        EXPECT_EQ(rows[i].values_[0].to_f64(), static_cast<double>(i + 1));
        EXPECT_EQ(rows[i].values_[1].to_f64(), static_cast<double>((i + 1) * 10));
    }
}

TEST_F(RecoveryTest, UncommittedRowsAreNotRecovered)
{
    ASSERT_TRUE(crash_after_writes(false));

    StorageEngine recovered(dir_.string(), 64);
    ASSERT_TRUE(recovered.init());

    EXPECT_TRUE(recovered.mvcc()->current_rows("t").empty());
}

TEST_F(RecoveryTest, WritesAfterRecoveryAreDurableAgain)
{
    ASSERT_TRUE(crash_after_writes(true));

    auto * second = new StorageEngine(dir_.string(), 64);
    ASSERT_TRUE(second->init());
    auto * txn = second->begin_txn();
    ASSERT_TRUE(second->mvcc()->stage_update(txn, "t", Value(ValueType::Int32, 7), make_row(7, 999)));
    ASSERT_TRUE(second->commit_txn(txn));
    second->buffer_pool()->invalidate_all();

    StorageEngine again(dir_.string(), 64);
    ASSERT_TRUE(again.init());
    auto rows = again.mvcc()->current_rows("t");
    ASSERT_EQ(rows.size(), 50u);
    auto updated = again.mvcc()->lookup_committed("t", Value(ValueType::Int32, 7));
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->values_[1].to_f64(), 999.0);
}

TEST_F(RecoveryTest, FlushingMidTransactionDoesNotLeakUncommittedRows)
{
    auto * engine = new StorageEngine(dir_.string(), 64);
    ASSERT_TRUE(engine->init());
    ASSERT_TRUE(engine->create_table("t", make_schema("t")));

    auto * committed = engine->begin_txn();
    ASSERT_TRUE(engine->mvcc()->stage_insert(committed, "t", make_row(1, 10)));
    ASSERT_TRUE(engine->commit_txn(committed));

    auto * open = engine->begin_txn();
    ASSERT_TRUE(engine->mvcc()->stage_insert(open, "t", make_row(2, 20)));
    // A steal policy would publish the open transaction's page effects here.
    ASSERT_TRUE(engine->flush_all());
    engine->buffer_pool()->invalidate_all();

    StorageEngine recovered(dir_.string(), 64);
    ASSERT_TRUE(recovered.init());
    auto rows = recovered.mvcc()->current_rows("t");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values_[0].to_f64(), 1.0);
}

} // namespace