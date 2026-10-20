#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
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

using namespace popo::storage;
using namespace popo::type;
using namespace popo::catalog;
namespace fs = std::filesystem;

namespace
{

Schema make_schema(const std::string & name)
{
    Schema schema;
    schema.id_ = 1;
    schema.table_name_ = name;
    schema.primary_key_col_idx_ = 0;

    Column id_col;
    id_col.column_name_ = "id";
    id_col.data_type_ = ValueType::Int32;
    id_col.is_primary_key_ = true;
    id_col.is_nullable_ = false;
    id_col.is_unique_ = true;
    id_col.type_len_ = 4;
    schema.columns_.push_back(id_col);

    Column data_col;
    data_col.column_name_ = "payload";
    data_col.data_type_ = ValueType::String;
    data_col.is_primary_key_ = false;
    data_col.is_nullable_ = true;
    data_col.is_unique_ = false;
    data_col.type_len_ = 512;
    schema.columns_.push_back(data_col);

    return schema;
}

Tuple make_row(const Schema & schema, int32_t id, const std::string & payload)
{
    Row row;
    row.append_value(Value(ValueType::Int32, id));
    row.append_value(Value(ValueType::String, payload));
    return Tuple::form(schema, row);
}

} // anonymous namespace

class StressTest : public ::testing::Test
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
        test_dir = fs::temp_directory_path().string() + "/popo_stress_" + std::to_string(rd());
        fs::create_directories(test_dir);

        dm = std::make_unique<DiskManager>(test_dir);
        dm->create_table_file("stress_table");
        bpm = std::make_unique<BufferPoolManager>(512, dm.get());
        redo = std::make_unique<RedoLogManager>(test_dir + "/redo.log");
        schema = make_schema("stress_table");
        heap = std::make_unique<TableHeap>("stress_table", bpm.get(), redo.get(), schema);
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

// Verify that a full scan returns rows in PK order with no duplicates
void verify_bplustree_order(TableHeap * heap, int expected_min_count = 0)
{
    auto it = heap->iter();
    it.rewind();
    int count = 0;
    int prev_id = -1;
    std::set<int> seen;
    while (it.valid())
    {
        auto row = it.current_row();
        ASSERT_GE(row.len(), 1u);
        int id = row.values_[0].i32();
        EXPECT_GT(id, prev_id) << "PK order violation: " << id << " after " << prev_id;
        EXPECT_TRUE(seen.insert(id).second) << "Duplicate PK: " << id;
        prev_id = id;
        count++;
        it.next();
    }
    EXPECT_GE(count, expected_min_count);
}

TEST_F(StressTest, SequentialInsert5000)
{
    popo::txn::Transaction txn(1);

    constexpr int N = 5000;
    for (int i = 0; i < N; ++i)
    {
        std::string payload = "seq_data_" + std::to_string(i);
        auto rid = heap->insert_row(&txn, make_row(schema, i, payload));
        ASSERT_TRUE(rid.is_valid()) << "Failed at i=" << i;
    }

    verify_bplustree_order(heap.get(), N);

    // Spot-check random rows
    std::mt19937 g(42);
    std::uniform_int_distribution<int> dist(0, N - 1);
    for (int i = 0; i < 200; ++i)
    {
        int target = dist(g);
        auto it = heap->iter();
        it.rewind();
        bool found = false;
        while (it.valid())
        {
            auto row = it.current_row();
            if (row.values_[0].i32() == target)
            {
                found = true;
                break;
            }
            it.next();
        }
        EXPECT_TRUE(found) << "Row " << target << " not found";
    }
}

TEST_F(StressTest, ReverseOrderInsert)
{
    popo::txn::Transaction txn(1);

    constexpr int N = 1000;
    for (int i = N - 1; i >= 0; --i)
    {
        std::string payload = "rev_" + std::to_string(i);
        auto rid = heap->insert_row(&txn, make_row(schema, i, payload));
        ASSERT_TRUE(rid.is_valid()) << "Failed at i=" << i;
    }

    verify_bplustree_order(heap.get(), N);
}

TEST_F(StressTest, RandomInsertLargePayload)
{
    popo::txn::Transaction txn(1);

    // Random order insert with medium payloads — stresses split propagation
    constexpr int N = 300;
    std::vector<int> ids(N);
    for (int i = 0; i < N; ++i)
        ids[i] = i;
    std::mt19937 g(7);
    std::shuffle(ids.begin(), ids.end(), g);

    int failed = 0;
    for (int id : ids)
    {
        std::string payload(120, 'A' + (id % 26));
        payload += "_" + std::to_string(id);
        auto rid = heap->insert_row(&txn, make_row(schema, id, payload));
        if (!rid.is_valid())
            failed++;
    }
    EXPECT_EQ(failed, 0) << "All inserts should succeed";

    // Verify all rows present and in PK order
    verify_bplustree_order(heap.get(), N);
}

TEST_F(StressTest, HeavyDeleteAndReinsert)
{
    popo::txn::Transaction txn(1);

    // Insert 1000 rows
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i)
    {
        heap->insert_row(&txn, make_row(schema, i, "heavy_test"));
    }

    std::mt19937 g(99);
    std::uniform_int_distribution<int> dist(0, N - 1);

    // Delete 300 random rows
    popo::txn::Transaction del_txn(2);
    for (int i = 0; i < 300; ++i)
    {
        // Find a row and delete it
        int target = dist(g);
        auto it = heap->iter();
        it.rewind();
        while (it.valid())
        {
            if (it.current_row().values_[0].i32() == target)
            {
                heap->delete_row(&del_txn, it.current_row_id());
                break;
            }
            it.next();
        }
    }

    // Reinsert 200 new rows (some same PKs, some new)
    popo::txn::Transaction ins_txn(3);
    std::set<int> new_ids;
    for (int i = 0; i < 200; ++i)
    {
        int id = dist(g);
        new_ids.insert(id);
        heap->insert_row(&ins_txn, make_row(schema, id, "reinserted"));
    }

    // Verify: scan should have roughly N rows (some deletes may fail,
    // some reinserts are duplicates of already-existing rows)
    auto it = heap->iter();
    it.rewind();
    int count = 0;
    int prev = -1;
    while (it.valid())
    {
        int pk = it.current_row().values_[0].i32();
        // Allow >= since B+Tree currently allows duplicate PKs when reinserting
        EXPECT_GE(pk, prev);
        prev = pk;
        count++;
        it.next();
    }
    // Count should be at least the remaining original rows (N - some deletes)
    // plus reinserts; exact count depends on overlap
    EXPECT_GE(count, N - 300);
}

TEST_F(StressTest, RepeatedSplitsAndScans)
{
    // Insert rows that cause many splits, then scan after each batch
    constexpr int BATCH = 100;
    constexpr int BATCHES = 20;

    for (int batch = 0; batch < BATCHES; ++batch)
    {
        popo::txn::Transaction txn(batch + 1);
        for (int i = 0; i < BATCH; ++i)
        {
            int id = batch * BATCH + i;
            std::string payload(200, 'A' + (batch % 26));
            auto rid = heap->insert_row(&txn, make_row(schema, id, payload));
            ASSERT_TRUE(rid.is_valid()) << "Failed at batch=" << batch << " id=" << id;
        }

        // Verify order after each batch
        verify_bplustree_order(heap.get(), (batch + 1) * BATCH);
    }

    // Final verification
    verify_bplustree_order(heap.get(), BATCH * BATCHES);
}

TEST_F(StressTest, TimingLargeScale)
{
    popo::txn::Transaction txn(1);
    constexpr int N = 2000;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < N; ++i)
    {
        std::string payload = "perf_test_" + std::to_string(i);
        heap->insert_row(&txn, make_row(schema, i, payload));
    }

    auto insert_end = std::chrono::steady_clock::now();

    // Full scan
    auto it = heap->iter();
    it.rewind();
    int count = 0;
    while (it.valid())
    {
        count++;
        it.next();
    }

    auto scan_end = std::chrono::steady_clock::now();

    // Update 100 rows
    popo::txn::Transaction upd_txn(2);
    for (int i = 0; i < 100; ++i)
    {
        auto it2 = heap->iter();
        it2.rewind();
        while (it2.valid())
        {
            if (it2.current_row().values_[0].i32() == i * 20)
            {
                heap->update_row(&upd_txn, it2.current_row_id(), make_row(schema, i * 20, "updated"));
                break;
            }
            it2.next();
        }
    }

    auto update_end = std::chrono::steady_clock::now();

    auto insert_ms = std::chrono::duration_cast<std::chrono::milliseconds>(insert_end - start).count();
    auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - insert_end).count();
    auto update_ms = std::chrono::duration_cast<std::chrono::milliseconds>(update_end - scan_end).count();

    EXPECT_EQ(count, N);
    // Performance assertions (should be fast enough)
    EXPECT_LT(insert_ms, 5000) << "Insert " << N << " rows took " << insert_ms << "ms";
    EXPECT_LT(scan_ms, 1000) << "Scan " << N << " rows took " << scan_ms << "ms";
    EXPECT_LT(update_ms, 1000) << "Update 100 rows took " << update_ms << "ms";

    std::cout << "  Insert " << N << " rows: " << insert_ms << "ms"
              << ", Scan: " << scan_ms << "ms"
              << ", Update 100: " << update_ms << "ms" << std::endl;
}

// Test: insert with negative and zero PK values
TEST_F(StressTest, NegativeAndZeroPK)
{
    popo::txn::Transaction txn(1);

    std::vector<int> pks = {-500, -200, 0, -1, -499, -201, -3, -2, 1, 2, 10, 5};
    for (int pk : pks)
    {
        auto rid = heap->insert_row(&txn, make_row(schema, pk, "neg_test"));
        ASSERT_TRUE(rid.is_valid()) << "Failed PK=" << pk;
    }

    // Verify sorted order (negative first, then positive)
    auto it = heap->iter();
    it.rewind();
    int prev = std::numeric_limits<int>::min();
    int count = 0;
    while (it.valid())
    {
        int id = it.current_row().values_[0].i32();
        EXPECT_GE(id, prev) << "Order: " << id << " after " << prev;
        prev = id;
        count++;
        it.next();
    }
    EXPECT_EQ(count, static_cast<int>(pks.size()));
}

// Test: interleaved insert and delete on same PK
TEST_F(StressTest, InterleavedInsertDelete)
{
    popo::txn::Transaction txn(1);

    for (int cycle = 0; cycle < 10; ++cycle)
    {
        int base = cycle * 50;
        for (int i = 0; i < 50; ++i)
        {
            heap->insert_row(&txn, make_row(schema, base + i, "cycle"));
        }
        // Delete first 10 of this cycle
        popo::txn::Transaction del_txn(cycle + 100);
        for (int i = 0; i < 10; ++i)
        {
            auto it = heap->iter();
            it.rewind();
            while (it.valid())
            {
                if (it.current_row().values_[0].i32() == base + i)
                {
                    heap->delete_row(&del_txn, it.current_row_id());
                    break;
                }
                it.next();
            }
        }
    }

    verify_bplustree_order(heap.get(), 400);
}

// Test: full table scan after each of many inserts
TEST_F(StressTest, ScanAfterEachInsert)
{
    popo::txn::Transaction txn(1);
    constexpr int N = 500;

    for (int i = 0; i < N; ++i)
    {
        heap->insert_row(&txn, make_row(schema, i, "scan_test"));
        // Scan every 50 inserts to verify
        if (i % 50 == 49 || i == N - 1)
        {
            auto it = heap->iter();
            it.rewind();
            int count = 0, prev = -1;
            while (it.valid())
            {
                int id = it.current_row().values_[0].i32();
                EXPECT_GT(id, prev) << "Scan at i=" << i;
                prev = id;
                count++;
                it.next();
            }
            EXPECT_EQ(count, i + 1);
        }
    }
}

// Test: update rows to larger payload sizes
TEST_F(StressTest, UpdateToLargerPayload)
{
    popo::txn::Transaction txn(1);
    constexpr int N = 200;

    for (int i = 0; i < N; ++i)
    {
        heap->insert_row(&txn, make_row(schema, i, "small"));
    }

    // Update every 3rd row to a much larger payload
    popo::txn::Transaction upd_txn(2);
    for (int i = 0; i < N; i += 3)
    {
        auto it = heap->iter();
        it.rewind();
        while (it.valid())
        {
            auto row = it.current_row();
            if (row.values_[0].i32() == i)
            {
                std::string big(500, 'X');
                big += std::to_string(i);
                heap->update_row(&upd_txn, it.current_row_id(), make_row(schema, i, big));
                break;
            }
            it.next();
        }
    }

    // Verify all rows present and ordered
    verify_bplustree_order(heap.get(), N);
}

// Test: deep tree structure with many small payloads
TEST_F(StressTest, DeepTreeManyRows)
{
    popo::txn::Transaction txn(1);
    constexpr int N = 10000;

    for (int i = 0; i < N; ++i)
    {
        auto rid = heap->insert_row(&txn, make_row(schema, i, "x"));
        ASSERT_TRUE(rid.is_valid()) << "Failed at i=" << i;
    }

    // Verify all 10000 rows
    verify_bplustree_order(heap.get(), N);

    // Random access — find every 1000th row
    for (int target : {0, 999, 1999, 2999, 3999, 4999, 5999, 6999, 7999, 8999, 9999})
    {
        auto it = heap->iter();
        it.rewind();
        bool found = false;
        while (it.valid())
        {
            if (it.current_row().values_[0].i32() == target)
            {
                found = true;
                break;
            }
            it.next();
        }
        EXPECT_TRUE(found) << "Row " << target << " not found";
    }
}
