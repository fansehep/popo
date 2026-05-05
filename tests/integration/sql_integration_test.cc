/**
 * SQL Integration Test: Compare Neon database results with MySQL 8.0
 *
 * Starts Neon server in background, executes identical SQL on both
 * Neon and MySQL 8.0 container, then compares results.
 */
#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include <mysql/mysql.h>
#include <spdlog/spdlog.h>
#include "catalog/schema_mgr.h"
#include "server/server.h"
#include "src/storage/storage_engine.h"

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class SQLIntegrationTest : public ::testing::Test
{
protected:
    // Neon server components
    std::unique_ptr<neon::catalog::SchemaManager> schema_mgr_;
    std::unique_ptr<neon::storage::StorageEngine> storage_engine_;
    std::unique_ptr<neon::server::Server> neon_server_;
    std::thread neon_thread_;

    // MySQL connections
    MYSQL * mysql_conn_ = nullptr;
    MYSQL * neon_conn_ = nullptr;

    static constexpr uint16_t NEON_PORT = 3307;

    void SetUp() override
    {
        // 1) Start Neon server in background thread
        schema_mgr_ = std::make_unique<neon::catalog::SchemaManager>();
        storage_engine_ = std::make_unique<neon::storage::StorageEngine>("test_data");
        storage_engine_->init();
        neon_server_ = std::make_unique<neon::server::Server>(NEON_PORT, *schema_mgr_, *storage_engine_);
        neon_thread_ = std::thread([this]() { neon_server_->start(); });
        // give the server a moment to bind
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 2) Connect to MySQL 8.0 container
        mysql_conn_ = mysql_init(nullptr);
        ASSERT_NE(mysql_conn_, nullptr);
        MYSQL * m = mysql_real_connect(mysql_conn_, "127.0.0.1", "root", "testpass", "testdb", 3306, nullptr, 0);
        ASSERT_NE(m, nullptr) << "MySQL 8.0 connect failed: " << mysql_error(mysql_conn_);

        // 3) Connect to Neon server
        neon_conn_ = mysql_init(nullptr);
        ASSERT_NE(neon_conn_, nullptr);
        MYSQL * n = mysql_real_connect(neon_conn_, "127.0.0.1", "root", "", "neon", NEON_PORT, nullptr, 0);
        // Neon may reject "neon" as db; retry without db if needed
        if (!n)
        {
            n = mysql_real_connect(neon_conn_, "127.0.0.1", "root", "", nullptr, NEON_PORT, nullptr, 0);
        }
        ASSERT_NE(n, nullptr) << "Neon connect failed: " << mysql_error(neon_conn_);
    }

    void TearDown() override
    {
        // Close connections
        if (mysql_conn_)
        {
            mysql_close(mysql_conn_);
            mysql_conn_ = nullptr;
        }
        if (neon_conn_)
        {
            mysql_close(neon_conn_);
            neon_conn_ = nullptr;
        }

        // Stop Neon server
        if (neon_server_)
        {
            neon_server_->stop();
        }
        if (neon_thread_.joinable())
        {
            neon_thread_.join();
        }
        // Brief delay to let detached session threads notice closed socket and exit
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // Shutdown storage engine
        if (storage_engine_)
        {
            storage_engine_->shutdown();
        }
    }

    // -------------------------------------------------------------------
    // Helper: compare two MySQL result sets
    // -------------------------------------------------------------------
    struct TableData
    {
        std::vector<std::string> columns;
        std::vector<std::vector<std::string>> rows;
    };

    static TableData FetchResult(MYSQL * conn)
    {
        TableData data;
        MYSQL_RES * res = mysql_store_result(conn);
        if (!res)
            return data;

        int ncols = mysql_num_fields(res);
        MYSQL_FIELD * fields = mysql_fetch_fields(res);
        for (int i = 0; i < ncols; ++i)
        {
            data.columns.push_back(fields[i].name ? fields[i].name : "");
        }

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)))
        {
            auto * lengths = mysql_fetch_lengths(res);
            std::vector<std::string> r;
            for (int i = 0; i < ncols; ++i)
            {
                if (row[i])
                {
                    r.emplace_back(row[i], lengths[i]);
                }
                else
                {
                    r.emplace_back("NULL");
                }
            }
            data.rows.push_back(std::move(r));
        }
        mysql_free_result(res);
        return data;
    }

    // Sort rows to handle result-order differences
    static void SortRows(TableData & data) { std::sort(data.rows.begin(), data.rows.end()); }

    static std::string DumpTable(const TableData & data)
    {
        std::ostringstream oss;
        oss << "Columns: ";
        for (size_t i = 0; i < data.columns.size(); ++i)
        {
            if (i > 0)
                oss << ", ";
            oss << data.columns[i];
        }
        oss << "\nRows (" << data.rows.size() << "):\n";
        for (const auto & row : data.rows)
        {
            for (size_t i = 0; i < row.size(); ++i)
            {
                if (i > 0)
                    oss << " | ";
                oss << row[i];
            }
            oss << "\n";
        }
        return oss.str();
    }

    // Execute SQL on both, compare sorted results
    void AssertSQL(const std::string & sql)
    {
        // Execute on MySQL
        int mysql_rc = mysql_query(mysql_conn_, sql.c_str());
        std::string mysql_err;
        if (mysql_rc != 0)
        {
            mysql_err = mysql_error(mysql_conn_);
            // If MySQL fails, Neon should also fail (skip comparison)
        }
        auto mysql_data = (mysql_rc == 0) ? FetchResult(mysql_conn_) : TableData{};

        // Execute on Neon
        int neon_rc = mysql_query(neon_conn_, sql.c_str());
        std::string neon_err;
        if (neon_rc != 0)
        {
            neon_err = mysql_error(neon_conn_);
        }
        auto neon_data = (neon_rc == 0) ? FetchResult(neon_conn_) : TableData{};

        // If MySQL failed, Neon should also fail
        if (mysql_rc != 0)
        {
            EXPECT_NE(neon_rc, 0) << "SQL [" << sql << "]\n"
                                  << "MySQL error: " << mysql_err << "\n"
                                  << "Neon unexpectedly succeeded.\n"
                                  << DumpTable(neon_data);
            return;
        }

        // Both succeeded: compare
        ASSERT_EQ(neon_rc, 0) << "SQL [" << sql << "]\n"
                              << "MySQL succeeded but Neon failed: " << neon_err;

        EXPECT_EQ(mysql_data.columns.size(), neon_data.columns.size())
            << "Column count mismatch for [" << sql << "]\n"
            << "MySQL: " << DumpTable(mysql_data) << "Neon: " << DumpTable(neon_data);

        EXPECT_EQ(mysql_data.rows.size(), neon_data.rows.size()) << "Row count mismatch for [" << sql << "]\n"
                                                                 << "MySQL: " << DumpTable(mysql_data) << "Neon: " << DumpTable(neon_data);

        // Sort both for order-independent comparison
        SortRows(mysql_data);
        SortRows(neon_data);

        EXPECT_EQ(mysql_data.rows, neon_data.rows) << "Result mismatch for [" << sql << "]\n"
                                                   << "MySQL: " << DumpTable(mysql_data) << "Neon: " << DumpTable(neon_data);
    }

    // Execute SQL on both, but only check that Neon doesn't crash
    void AssertSQLOk(const std::string & sql)
    {
        int neon_rc = mysql_query(neon_conn_, sql.c_str());
        if (neon_rc != 0)
        {
            ADD_FAILURE() << "Neon failed on [" << sql << "]: " << mysql_error(neon_conn_);
        }
    }
};

// ===========================================================================
// Test Cases
// ===========================================================================

// ---- DDL: CREATE TABLE / DROP TABLE ----

TEST_F(SQLIntegrationTest, CreateAndDropTable)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(50))");
    AssertSQL("DROP TABLE IF EXISTS t1");
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(50))");
    AssertSQL("DROP TABLE t1");
}

TEST_F(SQLIntegrationTest, CreateTableMultipleColumns)
{
    AssertSQL(
        "CREATE TABLE products ("
        "  id INT PRIMARY KEY,"
        "  name VARCHAR(100),"
        "  price DOUBLE,"
        "  quantity INT"
        ")");
    AssertSQL("DROP TABLE IF EXISTS products");
}

// ---- INSERT ----

TEST_F(SQLIntegrationTest, InsertSingleRow)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(50))");
    AssertSQL("INSERT INTO t1 (id, name) VALUES (1, 'Alice')");
    AssertSQL("SELECT * FROM t1");
    AssertSQL("DROP TABLE t1");
}

TEST_F(SQLIntegrationTest, InsertMultipleRows)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(50), age INT)");
    AssertSQL("INSERT INTO t1 (id, name, age) VALUES (1, 'Alice', 25)");
    AssertSQL("INSERT INTO t1 (id, name, age) VALUES (2, 'Bob', 30)");
    AssertSQL("INSERT INTO t1 (id, name, age) VALUES (3, 'Charlie', 35)");
    AssertSQL("SELECT * FROM t1");
    AssertSQL("SELECT id, name FROM t1");
    AssertSQL("SELECT age FROM t1");
    AssertSQL("DROP TABLE t1");
}

TEST_F(SQLIntegrationTest, InsertWithoutColumnList)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(50), age INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 'Alice', 25)");
    AssertSQL("INSERT INTO t1 VALUES (2, 'Bob', 30)");
    AssertSQL("SELECT * FROM t1");
    AssertSQL("DROP TABLE t1");
}

// ---- SELECT with WHERE ----

TEST_F(SQLIntegrationTest, SelectWhereEqual)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(50), age INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 'Alice', 25)");
    AssertSQL("INSERT INTO t1 VALUES (2, 'Bob', 30)");
    AssertSQL("INSERT INTO t1 VALUES (3, 'Charlie', 25)");
    AssertSQL("SELECT * FROM t1 WHERE name = 'Alice'");
    AssertSQL("SELECT * FROM t1 WHERE age = 25");
    AssertSQL("DROP TABLE t1");
}

TEST_F(SQLIntegrationTest, SelectWhereComparison)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, val INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 10)");
    AssertSQL("INSERT INTO t1 VALUES (2, 20)");
    AssertSQL("INSERT INTO t1 VALUES (3, 30)");
    AssertSQL("INSERT INTO t1 VALUES (4, 40)");
    AssertSQL("INSERT INTO t1 VALUES (5, 50)");
    AssertSQL("SELECT * FROM t1 WHERE val > 30");
    AssertSQL("SELECT * FROM t1 WHERE val < 30");
    AssertSQL("SELECT * FROM t1 WHERE val >= 20");
    AssertSQL("SELECT * FROM t1 WHERE val <= 20");
    AssertSQL("SELECT * FROM t1 WHERE val != 30");
    AssertSQL("DROP TABLE t1");
}

TEST_F(SQLIntegrationTest, SelectWhereAndOr)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(50), age INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 'Alice', 25)");
    AssertSQL("INSERT INTO t1 VALUES (2, 'Bob', 30)");
    AssertSQL("INSERT INTO t1 VALUES (3, 'Charlie', 35)");
    AssertSQL("INSERT INTO t1 VALUES (4, 'Diana', 30)");
    AssertSQL("SELECT * FROM t1 WHERE age > 25 AND name = 'Bob'");
    AssertSQL("SELECT * FROM t1 WHERE age < 30 OR age > 30");
    AssertSQL("DROP TABLE t1");
}

// ---- SELECT with ORDER BY ----

TEST_F(SQLIntegrationTest, SelectOrderBy)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, val INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 30)");
    AssertSQL("INSERT INTO t1 VALUES (2, 10)");
    AssertSQL("INSERT INTO t1 VALUES (3, 20)");
    AssertSQL("SELECT * FROM t1 ORDER BY val ASC");
    AssertSQL("SELECT * FROM t1 ORDER BY val DESC");
    AssertSQL("DROP TABLE t1");
}

// ---- SELECT with LIMIT ----

TEST_F(SQLIntegrationTest, SelectLimit)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, val INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 10)");
    AssertSQL("INSERT INTO t1 VALUES (2, 20)");
    AssertSQL("INSERT INTO t1 VALUES (3, 30)");
    AssertSQL("INSERT INTO t1 VALUES (4, 40)");
    AssertSQL("INSERT INTO t1 VALUES (5, 50)");
    AssertSQL("SELECT * FROM t1 LIMIT 3");
    AssertSQL("SELECT * FROM t1 ORDER BY val ASC LIMIT 2");
    AssertSQL("DROP TABLE t1");
}

// ---- UPDATE ----

TEST_F(SQLIntegrationTest, UpdateRows)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(50), age INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 'Alice', 25)");
    AssertSQL("INSERT INTO t1 VALUES (2, 'Bob', 30)");
    AssertSQL("UPDATE t1 SET age = 26 WHERE id = 1");
    AssertSQL("SELECT * FROM t1 WHERE id = 1");
    AssertSQL("UPDATE t1 SET name = 'Robert', age = 31 WHERE id = 2");
    AssertSQL("SELECT * FROM t1 WHERE id = 2");
    AssertSQL("DROP TABLE t1");
}

TEST_F(SQLIntegrationTest, UpdateAllRows)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, val INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 10)");
    AssertSQL("INSERT INTO t1 VALUES (2, 20)");
    AssertSQL("UPDATE t1 SET val = val + 1");
    AssertSQL("SELECT * FROM t1");
    AssertSQL("DROP TABLE t1");
}

// ---- DELETE ----

TEST_F(SQLIntegrationTest, DeleteRows)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(50))");
    AssertSQL("INSERT INTO t1 VALUES (1, 'Alice')");
    AssertSQL("INSERT INTO t1 VALUES (2, 'Bob')");
    AssertSQL("INSERT INTO t1 VALUES (3, 'Charlie')");
    AssertSQL("DELETE FROM t1 WHERE id = 2");
    AssertSQL("SELECT * FROM t1");
    AssertSQL("DELETE FROM t1");
    AssertSQL("SELECT * FROM t1");
    AssertSQL("DROP TABLE t1");
}

// ---- Aggregates ----

TEST_F(SQLIntegrationTest, AggregateCount)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, val INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 10)");
    AssertSQL("INSERT INTO t1 VALUES (2, 20)");
    AssertSQL("INSERT INTO t1 VALUES (3, 30)");
    AssertSQL("INSERT INTO t1 VALUES (4, 40)");
    AssertSQL("SELECT COUNT(*) FROM t1");
    AssertSQL("SELECT COUNT(val) FROM t1");
    AssertSQL("DROP TABLE t1");
}

TEST_F(SQLIntegrationTest, AggregateSum)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, val INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 10)");
    AssertSQL("INSERT INTO t1 VALUES (2, 20)");
    AssertSQL("INSERT INTO t1 VALUES (3, 30)");
    AssertSQL("SELECT SUM(val) FROM t1");
    AssertSQL("DROP TABLE t1");
}

TEST_F(SQLIntegrationTest, AggregateMinMax)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, val INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 30)");
    AssertSQL("INSERT INTO t1 VALUES (2, 10)");
    AssertSQL("INSERT INTO t1 VALUES (3, 20)");
    AssertSQL("SELECT MIN(val) FROM t1");
    AssertSQL("SELECT MAX(val) FROM t1");
    AssertSQL("DROP TABLE t1");
}

TEST_F(SQLIntegrationTest, AggregateAvg)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, val INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 10)");
    AssertSQL("INSERT INTO t1 VALUES (2, 20)");
    AssertSQL("INSERT INTO t1 VALUES (3, 30)");
    AssertSQL("SELECT AVG(val) FROM t1");
    AssertSQL("DROP TABLE t1");
}

// ---- Arithmetic expressions ----

TEST_F(SQLIntegrationTest, ArithmeticInSelect)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, a INT, b INT)");
    AssertSQL("INSERT INTO t1 VALUES (1, 10, 3)");
    AssertSQL("INSERT INTO t1 VALUES (2, 20, 4)");
    AssertSQL("SELECT a + b FROM t1");
    AssertSQL("SELECT a - b FROM t1");
    AssertSQL("SELECT a * b FROM t1");
    AssertSQL("SELECT a / b FROM t1");
    AssertSQL("DROP TABLE t1");
}

// ---- Data types ----

TEST_F(SQLIntegrationTest, DataTypesIntDoubleVarchar)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(100), score DOUBLE, active BOOLEAN)");
    AssertSQL("INSERT INTO t1 VALUES (1, 'Test', 95.5, TRUE)");
    AssertSQL("SELECT * FROM t1");
    AssertSQL("DROP TABLE t1");
}

TEST_F(SQLIntegrationTest, DataTypesBigIntSmallInt)
{
    AssertSQL(
        "CREATE TABLE t1 ("
        "  id INT PRIMARY KEY,"
        "  big_val BIGINT,"
        "  small_val SMALLINT,"
        "  tiny_val TINYINT"
        ")");
    AssertSQL("INSERT INTO t1 VALUES (1, 9223372036854775807, 32767, 127)");
    AssertSQL("SELECT * FROM t1");
    AssertSQL("DROP TABLE t1");
}

// ---- Large data test ----

TEST_F(SQLIntegrationTest, LargeDataset)
{
    AssertSQL("CREATE TABLE t1 (id INT PRIMARY KEY, val INT)");
    // Insert 100 rows
    for (int i = 1; i <= 100; ++i)
    {
        std::string sql = "INSERT INTO t1 VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")";
        AssertSQLOk(sql);
    }
    AssertSQL("SELECT COUNT(*) FROM t1");
    AssertSQL("SELECT * FROM t1 WHERE val > 500");
    AssertSQL("DROP TABLE t1");
}
