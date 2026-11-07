#include "sql_runner.h"

#include <filesystem>
#include <random>
#include <string>

#include <gtest/gtest.h>

namespace
{

class SqlSyntaxTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::random_device rd;
        dir_ = std::filesystem::temp_directory_path() / ("popo_syntax_test_" + std::to_string(rd()));
        std::filesystem::create_directories(dir_);
        runner_ = std::make_unique<popo::test::SqlRunner>(dir_.string());
    }

    void TearDown() override
    {
        runner_.reset();
        std::filesystem::remove_all(dir_);
    }

    popo::test::SqlResult must(const std::string & sql)
    {
        std::cerr << "[sql] " << sql << std::endl;
        auto result = runner_->exec(sql);
        EXPECT_TRUE(result.ok) << sql << " -> " << result.error;
        return result;
    }

    void seed_emp()
    {
        must("CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(16), dept VARCHAR(16), salary INT)");
        must("INSERT INTO emp VALUES (1, 'alice', 'eng', 100), (2, 'bob', 'eng', 200), (3, 'carol', 'sales', 150), (4, 'dave', 'sales', 150), (5, 'eve', 'hr', 300)");
    }

    std::filesystem::path dir_;
    std::unique_ptr<popo::test::SqlRunner> runner_;
};

TEST_F(SqlSyntaxTest, InAndNotIn)
{
    seed_emp();
    auto r = must("SELECT id FROM emp WHERE dept IN ('eng', 'hr')");
    ASSERT_EQ(r.result.rows.size(), 3u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 1);
    EXPECT_EQ(r.result.rows[1]->values_[0].i32(), 2);
    EXPECT_EQ(r.result.rows[2]->values_[0].i32(), 5);

    auto n = must("SELECT id FROM emp WHERE id NOT IN (1, 2, 3)");
    ASSERT_EQ(n.result.rows.size(), 2u);
    EXPECT_EQ(n.result.rows[0]->values_[0].i32(), 4);
    EXPECT_EQ(n.result.rows[1]->values_[0].i32(), 5);
}

TEST_F(SqlSyntaxTest, BetweenAndNotBetween)
{
    seed_emp();
    auto r = must("SELECT id FROM emp WHERE salary BETWEEN 150 AND 200");
    ASSERT_EQ(r.result.rows.size(), 3u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 2);
    EXPECT_EQ(r.result.rows[1]->values_[0].i32(), 3);
    EXPECT_EQ(r.result.rows[2]->values_[0].i32(), 4);

    auto n = must("SELECT id FROM emp WHERE salary NOT BETWEEN 150 AND 200");
    ASSERT_EQ(n.result.rows.size(), 2u);
    EXPECT_EQ(n.result.rows[0]->values_[0].i32(), 1);
    EXPECT_EQ(n.result.rows[1]->values_[0].i32(), 5);
}

TEST_F(SqlSyntaxTest, LikeAndNotLike)
{
    seed_emp();
    auto r = must("SELECT name FROM emp WHERE name LIKE 'a%'");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "alice");

    auto u = must("SELECT name FROM emp WHERE name LIKE '_ave'");
    ASSERT_EQ(u.result.rows.size(), 1u);
    EXPECT_EQ(u.result.rows[0]->values_[0].string_ref(), "dave");

    auto n = must("SELECT id FROM emp WHERE name NOT LIKE '%e%'");
    // alice, bob, carol contain 'e'? alice yes, bob no, carol no, dave yes, eve yes
    ASSERT_EQ(n.result.rows.size(), 2u);
    EXPECT_EQ(n.result.rows[0]->values_[0].i32(), 2);
    EXPECT_EQ(n.result.rows[1]->values_[0].i32(), 3);
}

TEST_F(SqlSyntaxTest, IsNullAndIsNotNull)
{
    must("CREATE TABLE t (id INT PRIMARY KEY, note VARCHAR(16))");
    must("INSERT INTO t VALUES (1, 'x'), (2, NULL), (3, 'y')");

    auto nulls = must("SELECT id FROM t WHERE note IS NULL");
    ASSERT_EQ(nulls.result.rows.size(), 1u);
    EXPECT_EQ(nulls.result.rows[0]->values_[0].i32(), 2);

    auto non_nulls = must("SELECT id FROM t WHERE note IS NOT NULL");
    ASSERT_EQ(non_nulls.result.rows.size(), 2u);
    EXPECT_EQ(non_nulls.result.rows[0]->values_[0].i32(), 1);
    EXPECT_EQ(non_nulls.result.rows[1]->values_[0].i32(), 3);

    // NULL never matches ordinary comparison
    auto eq = must("SELECT id FROM t WHERE note = 'x' OR note <> 'x'");
    ASSERT_EQ(eq.result.rows.size(), 2u);
}

TEST_F(SqlSyntaxTest, CaseWhen)
{
    seed_emp();
    auto searched = must("SELECT name, CASE WHEN salary >= 200 THEN 'high' WHEN salary >= 150 THEN 'mid' ELSE 'low' END FROM emp WHERE id <= 3");
    ASSERT_EQ(searched.result.rows.size(), 3u);
    EXPECT_EQ(searched.result.rows[0]->values_[1].string_ref(), "low");
    EXPECT_EQ(searched.result.rows[1]->values_[1].string_ref(), "high");
    EXPECT_EQ(searched.result.rows[2]->values_[1].string_ref(), "mid");

    auto simple = must("SELECT CASE dept WHEN 'eng' THEN 1 WHEN 'sales' THEN 2 ELSE 9 END FROM emp WHERE id = 5");
    ASSERT_EQ(simple.result.rows.size(), 1u);
    EXPECT_EQ(simple.result.rows[0]->values_[0].to_f64(), 9);
}

TEST_F(SqlSyntaxTest, SelectWithoutFromAndAlias)
{
    auto r = must("SELECT 1 + 2 AS total, 'hi' AS greeting");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].to_f64(), 3);
    EXPECT_EQ(r.result.rows[0]->values_[1].string_ref(), "hi");
    ASSERT_EQ(r.result.columns.size(), 2u);
    EXPECT_EQ(r.result.columns[0], "total");
    EXPECT_EQ(r.result.columns[1], "greeting");
}

TEST_F(SqlSyntaxTest, Distinct)
{
    seed_emp();
    auto r = must("SELECT DISTINCT dept FROM emp");
    ASSERT_EQ(r.result.rows.size(), 3u);

    auto s = must("SELECT DISTINCT dept, salary FROM emp");
    ASSERT_EQ(s.result.rows.size(), 4u); // (eng,100),(eng,200),(sales,150),(hr,300)
}

TEST_F(SqlSyntaxTest, LimitOffset)
{
    seed_emp();
    auto r = must("SELECT id FROM emp ORDER BY id LIMIT 2 OFFSET 2");
    ASSERT_EQ(r.result.rows.size(), 2u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 3);
    EXPECT_EQ(r.result.rows[1]->values_[0].i32(), 4);

    auto only_offset = must("SELECT id FROM emp ORDER BY id OFFSET 4");
    ASSERT_EQ(only_offset.result.rows.size(), 1u);
    EXPECT_EQ(only_offset.result.rows[0]->values_[0].i32(), 5);
}

TEST_F(SqlSyntaxTest, UnionAndUnionAll)
{
    seed_emp();
    auto u = must("SELECT dept FROM emp WHERE id <= 2 UNION SELECT dept FROM emp WHERE id >= 3");
    ASSERT_EQ(u.result.rows.size(), 3u); // eng, sales, hr

    auto ua = must("SELECT dept FROM emp WHERE id <= 2 UNION ALL SELECT dept FROM emp WHERE id >= 3");
    ASSERT_EQ(ua.result.rows.size(), 5u);

    auto ordered = must("SELECT id FROM emp WHERE id <= 2 UNION SELECT id FROM emp WHERE id >= 4 ORDER BY id DESC LIMIT 3");
    ASSERT_EQ(ordered.result.rows.size(), 3u);
    EXPECT_EQ(ordered.result.rows[0]->values_[0].i32(), 5);
    EXPECT_EQ(ordered.result.rows[1]->values_[0].i32(), 4);
    EXPECT_EQ(ordered.result.rows[2]->values_[0].i32(), 2);
}

TEST_F(SqlSyntaxTest, ScalarSubquery)
{
    seed_emp();
    auto r = must("SELECT name FROM emp WHERE salary = (SELECT MAX(salary) FROM emp)");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "eve");

    auto projected = must("SELECT (SELECT COUNT(*) FROM emp) AS n");
    ASSERT_EQ(projected.result.rows.size(), 1u);
    EXPECT_EQ(projected.result.rows[0]->values_[0].i64(), 5);
}

TEST_F(SqlSyntaxTest, InSubquery)
{
    must("CREATE TABLE dept (name VARCHAR(16) PRIMARY KEY, active INT)");
    must("INSERT INTO dept VALUES ('eng', 1), ('sales', 0), ('hr', 1)");
    seed_emp();

    auto r = must("SELECT id FROM emp WHERE dept IN (SELECT name FROM dept WHERE active = 1)");
    ASSERT_EQ(r.result.rows.size(), 3u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 1);
    EXPECT_EQ(r.result.rows[1]->values_[0].i32(), 2);
    EXPECT_EQ(r.result.rows[2]->values_[0].i32(), 5);

    auto n = must("SELECT id FROM emp WHERE dept NOT IN (SELECT name FROM dept WHERE active = 1)");
    ASSERT_EQ(n.result.rows.size(), 2u);
    EXPECT_EQ(n.result.rows[0]->values_[0].i32(), 3);
    EXPECT_EQ(n.result.rows[1]->values_[0].i32(), 4);
}

TEST_F(SqlSyntaxTest, ExistsAndCorrelatedSubquery)
{
    must("CREATE TABLE dept (name VARCHAR(16) PRIMARY KEY, active INT)");
    must("INSERT INTO dept VALUES ('eng', 1), ('hr', 1)");
    seed_emp();

    auto r = must("SELECT id FROM emp WHERE EXISTS (SELECT 1 FROM dept WHERE dept.name = emp.dept)");
    ASSERT_EQ(r.result.rows.size(), 3u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 1);
    EXPECT_EQ(r.result.rows[1]->values_[0].i32(), 2);
    EXPECT_EQ(r.result.rows[2]->values_[0].i32(), 5);

    auto correlated = must("SELECT name FROM emp WHERE salary > (SELECT AVG(salary) FROM emp e2 WHERE e2.dept = emp.dept)");
    // eng avg 150 -> bob; sales avg 150 -> none; hr avg 300 -> none
    ASSERT_EQ(correlated.result.rows.size(), 1u);
    EXPECT_EQ(correlated.result.rows[0]->values_[0].string_ref(), "bob");
}

TEST_F(SqlSyntaxTest, DerivedTable)
{
    seed_emp();
    auto r = must("SELECT d.dept FROM (SELECT dept, salary FROM emp WHERE salary >= 200) AS d");
    ASSERT_EQ(r.result.rows.size(), 2u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "eng");
    EXPECT_EQ(r.result.rows[1]->values_[0].string_ref(), "hr");
}

TEST_F(SqlSyntaxTest, WithCte)
{
    seed_emp();
    auto r = must("WITH high_paid AS (SELECT id, name FROM emp WHERE salary >= 200) SELECT name FROM high_paid");
    ASSERT_EQ(r.result.rows.size(), 2u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "bob");
    EXPECT_EQ(r.result.rows[1]->values_[0].string_ref(), "eve");

    auto two = must(
        "WITH a AS (SELECT id FROM emp WHERE id <= 2), b AS (SELECT id FROM emp WHERE id >= 4) "
        "SELECT id FROM a UNION SELECT id FROM b");
    ASSERT_EQ(two.result.rows.size(), 4u);
}

TEST_F(SqlSyntaxTest, GroupByHavingInExecutor)
{
    seed_emp();
    // DISTINCT forces the generic executor, which implements real GROUP BY
    auto r = must("SELECT DISTINCT dept, COUNT(*) AS n, SUM(salary) AS total FROM emp GROUP BY dept HAVING COUNT(*) > 1");
    ASSERT_EQ(r.result.rows.size(), 2u);

    bool seen_eng = false;
    bool seen_sales = false;
    for (const auto & row : r.result.rows)
    {
        auto dept = row->values_[0].string_ref();
        if (dept == "eng")
        {
            seen_eng = true;
            EXPECT_EQ(row->values_[1].i64(), 2);
            EXPECT_EQ(row->values_[2].to_f64(), 300);
        }
        else if (dept == "sales")
        {
            seen_sales = true;
            EXPECT_EQ(row->values_[1].i64(), 2);
            EXPECT_EQ(row->values_[2].to_f64(), 300);
        }
    }
    EXPECT_TRUE(seen_eng);
    EXPECT_TRUE(seen_sales);
}

TEST_F(SqlSyntaxTest, AlterTableAddColumn)
{
    seed_emp();
    must("ALTER TABLE emp ADD COLUMN bonus INT");
    auto r = must("SELECT id, bonus FROM emp WHERE id = 1");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_TRUE(r.result.rows[0]->values_[1].datatype() == popo::type::ValueType::Null);

    must("UPDATE emp SET bonus = 50 WHERE id = 1");
    auto u = must("SELECT bonus FROM emp WHERE id = 1");
    ASSERT_EQ(u.result.rows.size(), 1u);
    EXPECT_EQ(u.result.rows[0]->values_[0].to_f64(), 50);

    auto dup = runner_->exec("ALTER TABLE emp ADD COLUMN bonus INT");
    EXPECT_FALSE(dup.ok);
}

TEST_F(SqlSyntaxTest, AlterTableDropColumn)
{
    seed_emp();
    must("ALTER TABLE emp DROP COLUMN dept");
    auto r = must("SELECT * FROM emp WHERE id = 1");
    ASSERT_EQ(r.result.rows.size(), 1u);
    ASSERT_EQ(r.result.rows[0]->values_.size(), 3u);
    EXPECT_EQ(r.result.rows[0]->values_[1].string_ref(), "alice");
    EXPECT_EQ(r.result.rows[0]->values_[2].to_f64(), 100);

    auto pk = runner_->exec("ALTER TABLE emp DROP COLUMN id");
    EXPECT_FALSE(pk.ok);
}

TEST_F(SqlSyntaxTest, AlterTableRename)
{
    seed_emp();
    must("ALTER TABLE emp RENAME TO staff");
    auto r = must("SELECT COUNT(*) FROM staff");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i64(), 5);

    auto old = runner_->exec("SELECT * FROM emp");
    EXPECT_FALSE(old.ok);
}

TEST_F(SqlSyntaxTest, AlterTableKeepsSecondaryIndex)
{
    must("CREATE TABLE t (id INT PRIMARY KEY, k INT, name VARCHAR(16))");
    must("CREATE INDEX idx_k ON t (k)");
    must("INSERT INTO t VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 10, 'c')");

    must("ALTER TABLE t ADD COLUMN extra INT");
    auto r = must("SELECT name FROM t WHERE k = 10");
    ASSERT_EQ(r.result.rows.size(), 2u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "a");
    EXPECT_EQ(r.result.rows[1]->values_[0].string_ref(), "c");
}

TEST_F(SqlSyntaxTest, NewSyntaxInUpdateAndDelete)
{
    seed_emp();
    auto u = must("UPDATE emp SET salary = CASE WHEN dept = 'eng' THEN salary + 10 ELSE salary END WHERE dept IN ('eng')");
    EXPECT_EQ(u.result.rows_affected, 2);
    auto r = must("SELECT salary FROM emp WHERE id = 1");
    EXPECT_EQ(r.result.rows[0]->values_[0].to_f64(), 110);

    auto d = must("DELETE FROM emp WHERE name LIKE 'a%'");
    EXPECT_EQ(d.result.rows_affected, 1);
    auto left = must("SELECT COUNT(*) FROM emp");
    EXPECT_EQ(left.result.rows[0]->values_[0].i64(), 4);
}

} // namespace