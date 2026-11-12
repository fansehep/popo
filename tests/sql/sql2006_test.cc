#include "sql_runner.h"

#include <filesystem>
#include <random>
#include <string>

#include <gtest/gtest.h>

namespace
{

class Sql2006Test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::random_device rd;
        dir_ = std::filesystem::temp_directory_path() / ("popo_sql2006_" + std::to_string(rd()));
        std::filesystem::create_directories(dir_);
        runner_ = std::make_unique<popo::test::SqlRunner>(dir_.string());
    }

    void TearDown() override
    {
        runner_.reset();
        std::filesystem::remove_all(dir_);
    }

    popo::test::SqlSuccess must(const std::string & sql)
    {
        auto result = runner_->exec(sql);
        EXPECT_TRUE(result) << sql << " -> " << (result ? "" : result.error().error_message);
        return result ? std::move(*result) : popo::test::SqlSuccess{};
    }

    void seed()
    {
        must("CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(16), dept VARCHAR(16), salary INT)");
        must("INSERT INTO emp VALUES (1, 'alice', 'eng', 100), (2, 'bob', 'eng', 200), (3, 'carol', 'sales', 150), (4, 'dave', 'sales', 150), (5, 'eve', 'hr', 300)");
    }

    std::filesystem::path dir_;
    std::unique_ptr<popo::test::SqlRunner> runner_;
};

TEST_F(Sql2006Test, IntersectAndExcept)
{
    seed();
    auto both = must("SELECT id FROM emp WHERE id <= 3 INTERSECT SELECT id FROM emp WHERE id >= 2");
    ASSERT_EQ(both.result.rows.size(), 2u);
    EXPECT_EQ(both.result.rows[0]->values_[0].i32(), 2);
    EXPECT_EQ(both.result.rows[1]->values_[0].i32(), 3);

    auto diff = must("SELECT id FROM emp WHERE id <= 3 EXCEPT SELECT id FROM emp WHERE id = 2");
    ASSERT_EQ(diff.result.rows.size(), 2u);
    EXPECT_EQ(diff.result.rows[0]->values_[0].i32(), 1);
    EXPECT_EQ(diff.result.rows[1]->values_[0].i32(), 3);
}

TEST_F(Sql2006Test, FullOuterJoin)
{
    must("CREATE TABLE a (id INT PRIMARY KEY)");
    must("CREATE TABLE b (id INT PRIMARY KEY)");
    must("INSERT INTO a VALUES (1), (2)");
    must("INSERT INTO b VALUES (2), (3)");
    auto rows = must("SELECT a.id, b.id FROM a FULL JOIN b ON a.id = b.id");
    ASSERT_EQ(rows.result.rows.size(), 3u);
}

TEST_F(Sql2006Test, ScalarFunctionsAndCast)
{
    auto row = must("SELECT COALESCE(NULL, 2, 3), NULLIF(1, 1), NULLIF(1, 2), CAST('12' AS INT), UPPER('ab'), LOWER('CD'), CHAR_LENGTH('hi'), SUBSTRING('hello', 2, 3), TRIM('  x  '), ABS(-4), CONCAT('a', 'b'), 'a' || 'b'");
    ASSERT_EQ(row.result.rows.size(), 1u);
    const auto & v = row.result.rows[0]->values_;
    EXPECT_EQ(v[0].to_f64(), 2);
    EXPECT_TRUE(v[1].datatype() == popo::type::ValueType::Null);
    EXPECT_EQ(v[2].to_f64(), 1);
    EXPECT_EQ(v[3].to_f64(), 12);
    EXPECT_EQ(v[4].string_ref(), "AB");
    EXPECT_EQ(v[5].string_ref(), "cd");
    EXPECT_EQ(v[6].to_f64(), 2);
    EXPECT_EQ(v[7].string_ref(), "ell");
    EXPECT_EQ(v[8].string_ref(), "x");
    EXPECT_EQ(v[9].to_f64(), 4);
    EXPECT_EQ(v[10].string_ref(), "ab");
    EXPECT_EQ(v[11].string_ref(), "ab");
}

TEST_F(Sql2006Test, IsDistinctFrom)
{
    must("CREATE TABLE t (id INT PRIMARY KEY, note VARCHAR(16))");
    must("INSERT INTO t VALUES (1, 'x'), (2, NULL), (3, 'y')");
    auto distinct = must("SELECT id FROM t WHERE note IS DISTINCT FROM 'x'");
    ASSERT_EQ(distinct.result.rows.size(), 2u);
    EXPECT_EQ(distinct.result.rows[0]->values_[0].i32(), 2);
    EXPECT_EQ(distinct.result.rows[1]->values_[0].i32(), 3);

    auto same = must("SELECT id FROM t WHERE note IS NOT DISTINCT FROM NULL");
    ASSERT_EQ(same.result.rows.size(), 1u);
    EXPECT_EQ(same.result.rows[0]->values_[0].i32(), 2);
}

TEST_F(Sql2006Test, QuantifiedComparison)
{
    seed();
    auto all = must("SELECT id FROM emp WHERE salary > ALL (SELECT salary FROM emp WHERE dept = 'sales')");
    ASSERT_EQ(all.result.rows.size(), 2u);
    EXPECT_EQ(all.result.rows[0]->values_[0].i32(), 2);
    EXPECT_EQ(all.result.rows[1]->values_[0].i32(), 5);

    auto any = must("SELECT id FROM emp WHERE salary = ANY (SELECT salary FROM emp WHERE dept = 'hr')");
    ASSERT_EQ(any.result.rows.size(), 1u);
    EXPECT_EQ(any.result.rows[0]->values_[0].i32(), 5);
}

TEST_F(Sql2006Test, WindowFunctions)
{
    seed();
    auto ranked = must("SELECT name, ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary DESC) AS rn FROM emp WHERE dept = 'eng' ORDER BY salary DESC");
    ASSERT_EQ(ranked.result.rows.size(), 2u);
    EXPECT_EQ(ranked.result.rows[0]->values_[0].string_ref(), "bob");
    EXPECT_EQ(ranked.result.rows[0]->values_[1].i64(), 1);
    EXPECT_EQ(ranked.result.rows[1]->values_[0].string_ref(), "alice");
    EXPECT_EQ(ranked.result.rows[1]->values_[1].i64(), 2);

    auto total = must("SELECT total FROM (SELECT id, SUM(salary) OVER (PARTITION BY dept) AS total FROM emp) w WHERE id = 1");
    ASSERT_EQ(total.result.rows.size(), 1u);
    EXPECT_EQ(total.result.rows[0]->values_[0].to_f64(), 300);
}

TEST_F(Sql2006Test, RecursiveCte)
{
    auto rows = must("WITH RECURSIVE t AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM t WHERE n < 4) SELECT n FROM t");
    ASSERT_EQ(rows.result.rows.size(), 4u);
    EXPECT_EQ(rows.result.rows[0]->values_[0].to_f64(), 1);
    EXPECT_EQ(rows.result.rows[3]->values_[0].to_f64(), 4);
}

TEST_F(Sql2006Test, View)
{
    seed();
    must("CREATE VIEW high AS SELECT name FROM emp WHERE salary >= 200");
    auto rows = must("SELECT name FROM high");
    ASSERT_EQ(rows.result.rows.size(), 2u);
    EXPECT_EQ(rows.result.rows[0]->values_[0].string_ref(), "bob");
    EXPECT_EQ(rows.result.rows[1]->values_[0].string_ref(), "eve");
    must("DROP VIEW high");
    auto gone = runner_->exec("SELECT name FROM high");
    EXPECT_FALSE(gone);
}

TEST_F(Sql2006Test, SqlXmlPublishing)
{
    seed();
    auto one = must("SELECT XMLELEMENT(NAME item, name), XMLFOREST(name AS name, dept AS dept), XMLCOMMENT('hi') FROM emp WHERE id = 1");
    ASSERT_EQ(one.result.rows.size(), 1u);
    EXPECT_EQ(one.result.rows[0]->values_[0].string_ref(), "<item>alice</item>");
    EXPECT_EQ(one.result.rows[0]->values_[1].string_ref(), "<name>alice</name><dept>eng</dept>");
    EXPECT_EQ(one.result.rows[0]->values_[2].string_ref(), "<!--hi-->");

    auto grouped = must("SELECT XMLAGG(XMLELEMENT(NAME n, name)) FROM emp WHERE dept = 'eng'");
    ASSERT_EQ(grouped.result.rows.size(), 1u);
    EXPECT_EQ(grouped.result.rows[0]->values_[0].string_ref(), "<n>alice</n><n>bob</n>");
}

} // namespace
