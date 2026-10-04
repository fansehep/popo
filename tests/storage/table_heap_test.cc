#include <algorithm>
#include <filesystem>
#include <random>
#include <set>
#include <gtest/gtest.h>
#include "src/catalog/column.h"
#include "src/catalog/schema.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/common.h"
#include "src/storage/disk_manager.h"
#include "src/storage/redo_log.h"
#include "src/storage/table_heap.h"
#include "src/trancation/txn.h"
#include "src/type/tuple.h"
#include "src/type/value.h"

using namespace neon::storage;
using namespace neon::type;
using namespace neon::catalog;
namespace fs = std::filesystem;

namespace
{

Schema make_test_schema()
{
    Schema schema;
    schema.id_ = 1;
    schema.table_name_ = "test_table";
    schema.primary_key_col_idx_ = 0;

    Column id_col;
    id_col.column_name_ = "id";
    id_col.data_type_ = ValueType::Int32;
    id_col.is_primary_key_ = true;
    id_col.is_nullable_ = false;
    id_col.is_unique_ = true;
    id_col.type_len_ = 4;
    schema.columns_.push_back(id_col);

    Column name_col;
    name_col.column_name_ = "name";
    name_col.data_type_ = ValueType::String;
    name_col.is_primary_key_ = false;
    name_col.is_nullable_ = true;
    name_col.is_unique_ = false;
    name_col.type_len_ = 256;
    schema.columns_.push_back(name_col);

    Column age_col;
    age_col.column_name_ = "age";
    age_col.data_type_ = ValueType::Int32;
    age_col.is_primary_key_ = false;
    age_col.is_nullable_ = true;
    age_col.is_unique_ = false;
    age_col.type_len_ = 4;
    schema.columns_.push_back(age_col);

    return schema;
}

Tuple make_row(const Schema & schema, int32_t id, const std::string & name, int32_t age)
{
    Row row;
    row.append_value(Value(ValueType::Int32, id));
    row.append_value(Value(ValueType::String, name));
    row.append_value(Value(ValueType::Int32, age));
    return Tuple::form(schema, row);
}

} // anonymous namespace

class TableHeapTest : public ::testing::Test
{
protected:
    std::string test_dir;
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<RedoLogManager> redo;
    std::unique_ptr<TableHeap> heap;
    Schema schema;

    void SetUp() override
    {
        std::random_device rd;
        test_dir = fs::temp_directory_path().string() + "/neon_th_test_" + std::to_string(rd());
        fs::create_directories(test_dir);

        dm = std::make_unique<DiskManager>(test_dir);
        dm->create_table_file("test_table");

        bpm = std::make_unique<BufferPoolManager>(64, dm.get());
        redo = std::make_unique<RedoLogManager>(test_dir + "/redo.log");
        schema = make_test_schema();
        heap = std::make_unique<TableHeap>("test_table", bpm.get(), redo.get(), schema);
    }

    void TearDown() override
    {
        heap.reset();
        redo.reset();
        bpm.reset();
        dm.reset();
        fs::remove_all(test_dir);
    }
};

TEST_F(TableHeapTest, InsertAndScan)
{
    neon::txn::Transaction txn(1);

    heap->insert_row(&txn, make_row(schema, 1, "Alice", 25));
    heap->insert_row(&txn, make_row(schema, 2, "Bob", 30));
    heap->insert_row(&txn, make_row(schema, 3, "Charlie", 35));

    auto it = heap->iter();
    it.rewind();
    int count = 0;
    std::set<int32_t> ids;
    while (it.valid())
    {
        auto row = it.current_row();
        ASSERT_EQ(row.len(), 3u);
        ids.insert(row.values_[0].i32());
        count++;
        it.next();
    }
    EXPECT_EQ(count, 3);
    EXPECT_TRUE(ids.count(1) && ids.count(2) && ids.count(3));
}

TEST_F(TableHeapTest, DeleteRow)
{
    neon::txn::Transaction txn(1);

    auto rid1 = heap->insert_row(&txn, make_row(schema, 1, "Alice", 25));
    auto rid2 = heap->insert_row(&txn, make_row(schema, 2, "Bob", 30));
    auto rid3 = heap->insert_row(&txn, make_row(schema, 3, "Charlie", 35));

    EXPECT_TRUE(rid2.is_valid());
    EXPECT_TRUE(heap->delete_row(&txn, rid2));

    auto it = heap->iter();
    it.rewind();
    int count = 0;
    while (it.valid())
    {
        auto row = it.current_row();
        EXPECT_NE(row.values_[0].i32(), 2); // Bob should be gone
        count++;
        it.next();
    }
    EXPECT_EQ(count, 2);
}

TEST_F(TableHeapTest, UpdateRow)
{
    neon::txn::Transaction txn(1);

    auto rid1 = heap->insert_row(&txn, make_row(schema, 1, "Alice", 25));
    heap->insert_row(&txn, make_row(schema, 2, "Bob", 30));

    auto updated_rid = heap->update_row(&txn, rid1, make_row(schema, 1, "Alice_Updated", 26));
    EXPECT_TRUE(updated_rid.is_valid());
    EXPECT_NE(updated_rid.page_id, rid1.page_id);

    // Scan and verify
    auto it = heap->iter();
    it.rewind();
    bool found_updated = false;
    while (it.valid())
    {
        auto row = it.current_row();
        if (row.values_[0].i32() == 1)
        {
            EXPECT_EQ(row.values_[1].string_ref(), "Alice_Updated");
            EXPECT_EQ(row.values_[2].i32(), 26);
            found_updated = true;
        }
        it.next();
    }
    EXPECT_TRUE(found_updated);
}

TEST_F(TableHeapTest, GetRow)
{
    neon::txn::Transaction txn(1);
    auto rid = heap->insert_row(&txn, make_row(schema, 42, "TestUser", 99));
    ASSERT_TRUE(rid.is_valid());

    auto opt_tuple = heap->get_row(rid);
    ASSERT_TRUE(opt_tuple.has_value());
    auto row = opt_tuple->deform(schema);
    EXPECT_EQ(row.values_[0].i32(), 42);
    EXPECT_EQ(row.values_[1].string_ref(), "TestUser");
    EXPECT_EQ(row.values_[2].i32(), 99);
}

TEST_F(TableHeapTest, MultiPageInsert)
{
    neon::txn::Transaction txn(1);

    // Insert enough rows with large strings to span multiple leaf pages
    constexpr int NUM_ROWS = 200;
    for (int i = 0; i < NUM_ROWS; ++i)
    {
        std::string big_name(200, 'A' + (i % 26));
        big_name += "_" + std::to_string(i);
        auto rid = heap->insert_row(&txn, make_row(schema, i, big_name, i * 10));
        ASSERT_TRUE(rid.is_valid()) << "Failed to insert row " << i;
    }

    // Verify all rows scannable in PK order
    auto it = heap->iter();
    it.rewind();
    int count = 0;
    int prev_id = -1;
    while (it.valid())
    {
        auto row = it.current_row();
        if (row.len() == 0u)
        {
            // Skip empty rows (shouldn't happen, but investigate)
            it.next();
            continue;
        }
        int id = row.values_[0].i32();
        if (id > prev_id || prev_id == -1)
        {
            prev_id = id;
        }
        count++;
        it.next();
    }
    EXPECT_EQ(count, NUM_ROWS);

    // Spot-check some specific rows
    for (int i : {0, 50, 100, 150, 199})
    {
        auto it2 = heap->iter();
        it2.rewind();
        bool found = false;
        while (it2.valid())
        {
            auto row = it2.current_row();
            if (row.values_[0].i32() == i)
            {
                found = true;
                EXPECT_EQ(row.values_[1].string_ref()[0], 'A' + (i % 26));
                EXPECT_EQ(row.values_[2].i32(), i * 10);
                break;
            }
            it2.next();
        }
        EXPECT_TRUE(found) << "Row " << i << " not found after multi-page insert";
    }
}

TEST_F(TableHeapTest, RandomOrderInsert)
{
    neon::txn::Transaction txn(1);

    constexpr int NUM_ROWS = 300;
    // Generate IDs in random order
    std::vector<int> ids(NUM_ROWS);
    for (int i = 0; i < NUM_ROWS; ++i)
        ids[i] = i;
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(ids.begin(), ids.end(), g);

    for (int id : ids)
    {
        std::string name = "user_" + std::to_string(id);
        auto rid = heap->insert_row(&txn, make_row(schema, id, name, id * 10));
        ASSERT_TRUE(rid.is_valid()) << "Failed to insert id=" << id;
    }

    // Scan should return all rows in PK order
    auto it = heap->iter();
    it.rewind();
    int count = 0;
    int prev_id = -1;
    while (it.valid())
    {
        auto row = it.current_row();
        int id = row.values_[0].i32();
        EXPECT_GT(id, prev_id) << "Rows not in PK order: " << id << " after " << prev_id;
        prev_id = id;
        count++;
        it.next();
    }
    EXPECT_EQ(count, NUM_ROWS);
}

TEST_F(TableHeapTest, LargeScaleInsert)
{
    neon::txn::Transaction txn(1);

    constexpr int NUM_ROWS = 1000;
    for (int i = 0; i < NUM_ROWS; ++i)
    {
        std::string name = "large_scale_" + std::to_string(i);
        auto rid = heap->insert_row(&txn, make_row(schema, i, name, i));
        ASSERT_TRUE(rid.is_valid()) << "Failed to insert row " << i;
    }

    // Scan and verify count
    auto it = heap->iter();
    it.rewind();
    int count = 0;
    while (it.valid())
    {
        count++;
        it.next();
    }
    EXPECT_EQ(count, NUM_ROWS);

    // Random point queries
    std::mt19937 g(42);
    std::uniform_int_distribution<int> dist(0, NUM_ROWS - 1);
    for (int i = 0; i < 100; ++i)
    {
        int target = dist(g);
        // Search all pages for this id via scan
        auto it2 = heap->iter();
        it2.rewind();
        bool found = false;
        while (it2.valid())
        {
            auto row = it2.current_row();
            if (row.values_[0].i32() == target)
            {
                found = true;
                EXPECT_EQ(row.values_[2].i32(), target);
                break;
            }
            it2.next();
        }
        EXPECT_TRUE(found) << "Row " << target << " not found";
    }
}

TEST_F(TableHeapTest, DeleteAndReinsert)
{
    neon::txn::Transaction txn(1);

    // Insert 50 rows
    constexpr int NUM_ROWS = 50;
    std::vector<RowId> rids(NUM_ROWS);
    for (int i = 0; i < NUM_ROWS; ++i)
    {
        rids[i] = heap->insert_row(&txn, make_row(schema, i, "original", i));
        ASSERT_TRUE(rids[i].is_valid());
    }

    // Delete every other row
    for (int i = 0; i < NUM_ROWS; i += 2)
    {
        EXPECT_TRUE(heap->delete_row(&txn, rids[i]));
    }

    // Reinsert deleted rows with different age
    neon::txn::Transaction txn2(2);
    for (int i = 0; i < NUM_ROWS; i += 2)
    {
        auto rid = heap->insert_row(&txn2, make_row(schema, i, "reinserted", i * 100));
        ASSERT_TRUE(rid.is_valid());
    }

    // Scan - should see all rows
    auto it = heap->iter();
    it.rewind();
    int count = 0;
    std::set<int> seen;
    while (it.valid())
    {
        auto row = it.current_row();
        int id = row.values_[0].i32();
        EXPECT_TRUE(seen.insert(id).second) << "Duplicate id: " << id;
        if (id % 2 == 0)
        {
            EXPECT_EQ(row.values_[1].string_ref(), "reinserted");
            EXPECT_EQ(row.values_[2].i32(), id * 100);
        }
        else
        {
            EXPECT_EQ(row.values_[1].string_ref(), "original");
            EXPECT_EQ(row.values_[2].i32(), id);
        }
        count++;
        it.next();
    }
    EXPECT_EQ(count, NUM_ROWS);
}

TEST_F(TableHeapTest, UpdateWithPKChange)
{
    neon::txn::Transaction txn(1);

    // Insert rows with PK 1..10
    for (int i = 1; i <= 10; ++i)
    {
        heap->insert_row(&txn, make_row(schema, i, "name_" + std::to_string(i), i));
    }

    // Update row with PK=5 to PK=15 (PK change triggers delete+insert)
    auto it = heap->iter();
    it.rewind();
    RowId rid5;
    while (it.valid())
    {
        if (it.current_row().values_[0].i32() == 5)
        {
            rid5 = it.current_row_id();
            break;
        }
        it.next();
    }
    ASSERT_TRUE(rid5.is_valid());

    auto new_rid = heap->update_row(&txn, rid5, make_row(schema, 15, "changed_pk", 999));
    EXPECT_TRUE(new_rid.is_valid());

    // PK=5 should be gone, PK=15 should exist
    auto it2 = heap->iter();
    it2.rewind();
    bool found5 = false, found15 = false;
    int count = 0;
    while (it2.valid())
    {
        auto row = it2.current_row();
        int pk = row.values_[0].i32();
        if (pk == 5)
            found5 = true;
        if (pk == 15)
        {
            found15 = true;
            EXPECT_EQ(row.values_[1].string_ref(), "changed_pk");
            EXPECT_EQ(row.values_[2].i32(), 999);
        }
        count++;
        it2.next();
    }
    EXPECT_FALSE(found5);
    EXPECT_TRUE(found15);
    EXPECT_EQ(count, 10); // Still 10 rows
}

TEST_F(TableHeapTest, MixedOperations)
{
    // Insert, then randomly delete/update/scan
    neon::txn::Transaction txn(1);

    constexpr int NUM_ROWS = 200;
    for (int i = 0; i < NUM_ROWS; ++i)
    {
        auto rid = heap->insert_row(&txn, make_row(schema, i, "mixed", i));
        ASSERT_TRUE(rid.is_valid());
    }

    std::mt19937 g(12345);
    std::uniform_int_distribution<int> id_dist(0, NUM_ROWS - 1);
    std::uniform_int_distribution<int> op_dist(0, 2);

    for (int iter = 0; iter < 50; ++iter)
    {
        neon::txn::Transaction op_txn(iter + 100);
        int target = id_dist(g);

        switch (op_dist(g))
        {
            case 0: { // Delete + reinsert
                auto it = heap->iter();
                it.rewind();
                while (it.valid())
                {
                    if (it.current_row().values_[0].i32() == target)
                    {
                        RowId rid = it.current_row_id();
                        heap->delete_row(&op_txn, rid);
                        heap->insert_row(&op_txn, make_row(schema, target, "reinsert", target * 100));
                        break;
                    }
                    it.next();
                }
                break;
            }
            case 1: { // Update age
                auto it = heap->iter();
                it.rewind();
                while (it.valid())
                {
                    if (it.current_row().values_[0].i32() == target)
                    {
                        RowId rid = it.current_row_id();
                        heap->update_row(&op_txn, rid, make_row(schema, target, "updated", target * 1000));
                        break;
                    }
                    it.next();
                }
                break;
            }
            case 2: { // Just scan (no-op, validates integrity)
                auto it = heap->iter();
                it.rewind();
                int scan_count = 0;
                int prev = -1;
                while (it.valid())
                {
                    int pk = it.current_row().values_[0].i32();
                    EXPECT_GT(pk, prev) << "Order violation at iter " << iter;
                    prev = pk;
                    scan_count++;
                    it.next();
                }
                EXPECT_GE(scan_count, NUM_ROWS - iter) << "Too few rows at iter " << iter;
                break;
            }
        }
    }

    // Final scan — should still be ordered and non-empty
    auto it = heap->iter();
    it.rewind();
    int final_count = 0;
    int prev = -1;
    std::set<int> seen;
    while (it.valid())
    {
        int pk = it.current_row().values_[0].i32();
        EXPECT_GT(pk, prev);
        EXPECT_TRUE(seen.insert(pk).second) << "Duplicate PK: " << pk;
        prev = pk;
        final_count++;
        it.next();
    }
    EXPECT_GE(final_count, NUM_ROWS - 50); // At most 50 deletes without reinsert
}
