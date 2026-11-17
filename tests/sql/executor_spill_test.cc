#include "executor/spill.h"
#include "sql_runner.h"

#include <algorithm>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace
{

using popo::executor::MemoryTracker;
using popo::executor::SpillReader;
using popo::executor::SpillWriter;
using popo::executor::TempFileManager;
using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

Row make_row(int64_t id, std::string text)
{
    Row row;
    row.append_value(Value(ValueType::Int64, id));
    row.append_value(Value(ValueType::String, text));
    row.append_value(Value());
    row.append_value(Value(ValueType::Bool, (id % 2) != 0));
    row.append_value(Value(ValueType::Float64, static_cast<double>(id) / 10.0));
    return row;
}

TEST(ExecutorSpill, TypedCodecRoundTrips)
{
    auto root = std::filesystem::temp_directory_path() / "popodb_spill_codec_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    TempFileManager temp(root);
    auto path = temp.create("codec");
    ASSERT_TRUE(path) << (path ? "" : path.error().error_message);
    SpillWriter writer(*path, &temp);
    ASSERT_TRUE(writer.write(make_row(-42, std::string("a\0b", 3))));
    ASSERT_TRUE(writer.close());

    SpillReader reader(*path);
    auto decoded = reader.next();
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(*decoded);
    EXPECT_EQ((**decoded).values_[0].i64(), -42);
    EXPECT_EQ((**decoded).values_[1].string_ref(), std::string("a\0b", 3));
    EXPECT_EQ((**decoded).values_[2].datatype(), ValueType::Null);
    EXPECT_FALSE((**decoded).values_[3].boolean());
    EXPECT_DOUBLE_EQ((**decoded).values_[4].f64(), -4.2);
    auto eof = reader.next();
    ASSERT_TRUE(eof);
    EXPECT_FALSE(*eof);
}

TEST(ExecutorSpill, ExternalMergeSortMatchesStableInMemorySortAt64KiB)
{
    auto root = std::filesystem::temp_directory_path() / "popodb_spill_sort_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::filesystem::path query_dir;
    std::vector<Row> input;
    for (int i = 0; i < 4000; ++i)
        input.push_back(make_row((i * 7919) % 101, std::string(80, static_cast<char>('a' + i % 26))));
    auto expected = input;
    auto less = [](const Row & a, const Row & b) { return a.values_[0].i64() < b.values_[0].i64(); };
    std::stable_sort(expected.begin(), expected.end(), less);

    {
        MemoryTracker memory(64 * 1024);
        TempFileManager temp(root);
        query_dir = temp.directory();
        size_t cursor = 0;
        auto sorted = popo::executor::external_sort(
            [&]() -> popo::base::Result<std::optional<Row>>
            {
                if (cursor == input.size())
                    return std::optional<Row>{};
                return std::optional<Row>(std::move(input[cursor++]));
            },
            less,
            memory,
            temp);
        ASSERT_TRUE(sorted) << (sorted ? "" : sorted.error().error_message);
        ASSERT_EQ(sorted->size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i)
        {
            EXPECT_EQ((*sorted)[i].values_[0].i64(), expected[i].values_[0].i64());
            EXPECT_EQ((*sorted)[i].values_[1].string_ref(), expected[i].values_[1].string_ref());
        }
        EXPECT_GT(temp.files_created(), 1u);
        EXPECT_GT(temp.bytes_written(), 0u);
        EXPECT_LE(memory.peak(), memory.limit());
    }
    EXPECT_FALSE(std::filesystem::exists(query_dir));
    std::filesystem::remove_all(root);
}

TEST(ExecutorSpill, PartitionFilesCleanedOnError)
{
    auto root = std::filesystem::temp_directory_path() / "popodb_spill_error_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::filesystem::path query_dir;
    {
        TempFileManager temp(root);
        query_dir = temp.directory();
        size_t cursor = 0;
        auto partitioned = popo::executor::partition_rows(
            [&]() -> popo::base::Result<std::optional<Row>>
            {
                if (cursor++ == 20)
                    return popo::base::make_error(popo::base::Errno::InternalErr, "injected failure");
                return std::optional<Row>(make_row(static_cast<int64_t>(cursor), "payload"));
            },
            [](const Row & row) { return std::hash<int64_t>{}(row.values_[0].i64()); },
            8,
            temp,
            "error");
        ASSERT_FALSE(partitioned);
        EXPECT_EQ(partitioned.error().error_message, "injected failure");
        EXPECT_TRUE(std::filesystem::exists(query_dir));
    }
    EXPECT_FALSE(std::filesystem::exists(query_dir));
    std::filesystem::remove_all(root);
}

TEST(ExecutorSpill, Forced64KiBQueryMatchesNonSpillAndCleansFiles)
{
    auto root = std::filesystem::temp_directory_path() / "popodb_spill_query_test";
    auto database = root / "database";
    auto temporary = root / "temporary";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(temporary);
    popo::test::SqlRunner db(database.string());
    ASSERT_TRUE(db.exec("CREATE TABLE spill_data (id INT PRIMARY KEY, grp INT, payload VARCHAR(128), v INT)"));
    auto transaction = db.begin();
    ASSERT_TRUE(transaction);
    for (int i = 0; i < 1600; ++i)
    {
        auto inserted = transaction->exec(
            "INSERT INTO spill_data VALUES (" + std::to_string(i) + ", " + std::to_string(i % 17) + ", 'payload-"
            + std::to_string(i % 311) + std::string(48, 'x') + "', " + std::to_string(i % 23) + ")");
        ASSERT_TRUE(inserted) << (inserted ? "" : inserted.error().error_message);
    }
    ASSERT_TRUE(transaction->commit());

    const std::string query = "SELECT grp, COUNT(DISTINCT payload) AS distinct_payloads, SUM(v) AS total "
                              "FROM spill_data GROUP BY grp ORDER BY grp";
    db.set_executor_options({8 * 1024 * 1024, temporary});
    auto expected = db.exec(query);
    ASSERT_TRUE(expected) << (expected ? "" : expected.error().error_message);

    db.set_executor_options({64 * 1024, temporary});
    auto spilled = db.exec(query);
    ASSERT_TRUE(spilled) << (spilled ? "" : spilled.error().error_message);
    EXPECT_LE(spilled->spill.peak_memory_bytes, 64 * 1024u);
    EXPECT_GT(spilled->spill.files_created, 0u);
    EXPECT_GT(spilled->spill.bytes_written, 0u);
    RecordProperty("aggregate_peak_bytes", spilled->spill.peak_memory_bytes);
    RecordProperty("aggregate_spill_files", spilled->spill.files_created);
    RecordProperty("aggregate_spill_bytes", spilled->spill.bytes_written);
    ASSERT_EQ(spilled->result.rows.size(), expected->result.rows.size());
    for (size_t i = 0; i < expected->result.rows.size(); ++i)
        EXPECT_EQ(spilled->result.rows[i]->to_string(), expected->result.rows[i]->to_string());
    EXPECT_TRUE(std::filesystem::is_empty(temporary));

    const std::string join_query = "SELECT a.grp, COUNT(*) AS matches "
                                   "FROM spill_data a JOIN spill_data b ON a.id = b.id "
                                   "GROUP BY a.grp ORDER BY a.grp";
    db.set_executor_options({8 * 1024 * 1024, temporary});
    auto expected_join = db.exec(join_query);
    ASSERT_TRUE(expected_join) << (expected_join ? "" : expected_join.error().error_message);
    db.set_executor_options({64 * 1024, temporary});
    auto spilled_join = db.exec(join_query);
    ASSERT_TRUE(spilled_join) << (spilled_join ? "" : spilled_join.error().error_message);
    EXPECT_LE(spilled_join->spill.peak_memory_bytes, 64 * 1024u);
    EXPECT_GT(spilled_join->spill.files_created, 0u);
    EXPECT_GT(spilled_join->spill.bytes_written, 0u);
    RecordProperty("join_peak_bytes", spilled_join->spill.peak_memory_bytes);
    RecordProperty("join_spill_files", spilled_join->spill.files_created);
    RecordProperty("join_spill_bytes", spilled_join->spill.bytes_written);
    ASSERT_EQ(spilled_join->result.rows.size(), expected_join->result.rows.size());
    for (size_t i = 0; i < expected_join->result.rows.size(); ++i)
        EXPECT_EQ(spilled_join->result.rows[i]->to_string(), expected_join->result.rows[i]->to_string());
    EXPECT_TRUE(std::filesystem::is_empty(temporary));

    const std::string cte_query = "WITH shared_rows AS (SELECT id, grp FROM spill_data) "
                                  "SELECT COUNT(*) FROM shared_rows a "
                                  "JOIN shared_rows b ON a.id = b.id";
    db.set_executor_options({8 * 1024 * 1024, temporary});
    auto expected_cte = db.exec(cte_query);
    ASSERT_TRUE(expected_cte) << (expected_cte ? "" : expected_cte.error().error_message);
    db.set_executor_options({64 * 1024, temporary});
    auto spilled_cte = db.exec(cte_query);
    ASSERT_TRUE(spilled_cte) << (spilled_cte ? "" : spilled_cte.error().error_message);
    EXPECT_LE(spilled_cte->spill.peak_memory_bytes, 64 * 1024u);
    EXPECT_GT(spilled_cte->spill.files_created, 0u);
    EXPECT_GT(spilled_cte->spill.bytes_written, 0u);
    RecordProperty("cte_peak_bytes", spilled_cte->spill.peak_memory_bytes);
    RecordProperty("cte_spill_files", spilled_cte->spill.files_created);
    RecordProperty("cte_spill_bytes", spilled_cte->spill.bytes_written);
    ASSERT_EQ(spilled_cte->result.rows.size(), 1u);
    EXPECT_EQ(spilled_cte->result.rows[0]->to_string(), expected_cte->result.rows[0]->to_string());
    EXPECT_TRUE(std::filesystem::is_empty(temporary));

    auto failed = db.exec(
        "SELECT DISTINCT payload FROM spill_data "
        "ORDER BY CAST(payload AS INTEGER)");
    ASSERT_FALSE(failed);
    EXPECT_TRUE(std::filesystem::is_empty(temporary));
    std::filesystem::remove_all(root);
}

} // namespace
