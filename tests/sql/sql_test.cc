#include "sql_runner.h"

#include <filesystem>
#include <random>
#include <string>

#include <gtest/gtest.h>

namespace
{

class SqlExecTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::random_device rd;
        dir_ = std::filesystem::temp_directory_path() / ("neon_sql_test_" + std::to_string(rd()));
        std::filesystem::create_directories(dir_);
        runner_ = std::make_unique<neon::test::SqlRunner>(dir_.string());
    }

    void TearDown() override
    {
        runner_.reset();
        std::filesystem::remove_all(dir_);
    }

    neon::test::SqlResult must(const std::string & sql)
    {
        auto result = runner_->exec(sql);
        EXPECT_TRUE(result.ok) << sql << " -> " << result.error;
        return result;
    }

    std::filesystem::path dir_;
    std::unique_ptr<neon::test::SqlRunner> runner_;
};

TEST_F(SqlExecTest, CreateInsertSelect)
{
    must("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16))");
    auto inserted = must("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");
    EXPECT_EQ(inserted.result.rows_affected, 3);

    auto scanned = must("SELECT * FROM t");
    ASSERT_EQ(scanned.result.rows.size(), 3u);
    EXPECT_EQ(scanned.result.rows[0]->values_[0].i32(), 1);
    EXPECT_EQ(scanned.result.rows[0]->values_[1].string_ref(), "a");
    EXPECT_EQ(scanned.result.rows[1]->values_[0].i32(), 2);
    EXPECT_EQ(scanned.result.rows[2]->values_[0].i32(), 3);
}

TEST_F(SqlExecTest, SelectWhere)
{
    must("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16))");
    must("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");

    auto found = must("SELECT name FROM t WHERE id = 2");
    ASSERT_EQ(found.result.rows.size(), 1u);
    EXPECT_EQ(found.result.rows[0]->values_[0].string_ref(), "b");

    auto missing = must("SELECT name FROM t WHERE id = 9");
    EXPECT_TRUE(missing.result.rows.empty());
}

TEST_F(SqlExecTest, UpdateWhere)
{
    must("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16))");
    must("INSERT INTO t VALUES (1, 'a'), (2, 'b')");

    auto updated = must("UPDATE t SET name = 'zz' WHERE id = 2");
    EXPECT_EQ(updated.result.rows_affected, 1);

    auto found = must("SELECT name FROM t WHERE id = 2");
    ASSERT_EQ(found.result.rows.size(), 1u);
    EXPECT_EQ(found.result.rows[0]->values_[0].string_ref(), "zz");

    auto other = must("SELECT name FROM t WHERE id = 1");
    ASSERT_EQ(other.result.rows.size(), 1u);
    EXPECT_EQ(other.result.rows[0]->values_[0].string_ref(), "a");
}

TEST_F(SqlExecTest, DeleteWhere)
{
    must("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16))");
    must("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");

    auto deleted = must("DELETE FROM t WHERE id = 2");
    EXPECT_EQ(deleted.result.rows_affected, 1);

    auto scanned = must("SELECT id FROM t");
    ASSERT_EQ(scanned.result.rows.size(), 2u);
    EXPECT_EQ(scanned.result.rows[0]->values_[0].i32(), 1);
    EXPECT_EQ(scanned.result.rows[1]->values_[0].i32(), 3);
}

TEST_F(SqlExecTest, SecondaryIndexAndPkRange)
{
    must("CREATE TABLE t (id INT PRIMARY KEY, k INT, name VARCHAR(16))");
    must("CREATE INDEX idx_k ON t (k)");
    must("INSERT INTO t VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 10, 'c'), (4, 30, 'd')");

    auto both = must("SELECT name FROM t WHERE k = 10");
    ASSERT_EQ(both.result.rows.size(), 2u);
    EXPECT_EQ(both.result.rows[0]->values_[0].string_ref(), "a");
    EXPECT_EQ(both.result.rows[1]->values_[0].string_ref(), "c");

    auto ranged = must("SELECT id FROM t WHERE id >= 2 AND id <= 3");
    ASSERT_EQ(ranged.result.rows.size(), 2u);
    EXPECT_EQ(ranged.result.rows[0]->values_[0].i32(), 2);
    EXPECT_EQ(ranged.result.rows[1]->values_[0].i32(), 3);

    must("UPDATE t SET k = 15 WHERE id = 1");
    auto after = must("SELECT name FROM t WHERE k = 10");
    ASSERT_EQ(after.result.rows.size(), 1u);
    EXPECT_EQ(after.result.rows[0]->values_[0].string_ref(), "c");
    auto moved = must("SELECT name FROM t WHERE k = 15");
    ASSERT_EQ(moved.result.rows.size(), 1u);
    EXPECT_EQ(moved.result.rows[0]->values_[0].string_ref(), "a");

    must("DELETE FROM t WHERE id = 3");
    auto gone = must("SELECT name FROM t WHERE k = 10");
    EXPECT_TRUE(gone.result.rows.empty());

    for (int i = 0; i < 20; ++i)
    {
        auto point = must("SELECT name FROM t WHERE id = 2");
        ASSERT_EQ(point.result.rows.size(), 1u);
        EXPECT_EQ(point.result.rows[0]->values_[0].string_ref(), "b");
    }
}

TEST_F(SqlExecTest, AggregateFilterAndJoin)
{
    must("CREATE TABLE lineitem (id INT PRIMARY KEY, order_id INT, quantity INT, price INT, discount INT)");
    must("CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, priority INT)");
    must("INSERT INTO orders VALUES (1, 10, 1), (2, 11, 2), (3, 12, 1)");
    must("INSERT INTO lineitem VALUES (1, 1, 5, 100, 1), (2, 1, 9, 200, 4), (3, 2, 3, 50, 0), (4, 3, 8, 80, 3)");

    auto filtered = must("SELECT SUM(price) FROM lineitem WHERE discount > 2");
    ASSERT_EQ(filtered.result.rows.size(), 1u);
    EXPECT_EQ(filtered.result.rows[0]->values_[0].i64(), 280);

    auto joined = must("SELECT lineitem.id FROM lineitem JOIN orders ON lineitem.order_id = orders.id WHERE orders.priority = 1");
    ASSERT_EQ(joined.result.rows.size(), 3u);
    EXPECT_EQ(joined.result.rows[0]->values_[0].i64(), 1);
    EXPECT_EQ(joined.result.rows[1]->values_[0].i64(), 2);
    EXPECT_EQ(joined.result.rows[2]->values_[0].i64(), 4);
}

TEST_F(SqlExecTest, CountStar)
{
    must("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16))");
    must("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");

    auto counted = must("SELECT COUNT(*) FROM t");
    ASSERT_EQ(counted.result.rows.size(), 1u);
    EXPECT_EQ(counted.result.rows[0]->values_[0].i64(), 3);
}

} // namespace
