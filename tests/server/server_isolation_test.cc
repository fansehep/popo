// End-to-end isolation tests over the MySQL wire protocol. Two real client
// connections drive the SQL that only Session implements: BEGIN/COMMIT/
// ROLLBACK, SET autocommit and the SET ... TRANSACTION ISOLATION LEVEL scopes.
#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <mysql/mysql.h>
#include <spdlog/spdlog.h>

#include "catalog/schema_mgr.h"
#include "server/server.h"
#include "src/storage/storage_engine.h"

namespace
{

class ServerIsolationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        spdlog::set_level(spdlog::level::err);
        std::random_device rd;
        dir_ = std::filesystem::temp_directory_path() / ("popo_server_iso_" + std::to_string(rd()));
        std::filesystem::create_directories(dir_);

        schema_mgr_ = std::make_unique<popo::catalog::SchemaManager>();
        engine_ = std::make_unique<popo::storage::StorageEngine>(dir_.string());
        auto initialized = engine_->init();
        ASSERT_TRUE(initialized) << initialized.error().error_message;
        // Port 0 binds an ephemeral port, so concurrent runs cannot collide.
        auto server = popo::server::Server::create(0, *schema_mgr_, *engine_);
        ASSERT_TRUE(server) << server.error().error_message;
        server_ = std::move(*server);
        auto port = server_->port();
        ASSERT_TRUE(port) << port.error().error_message;
        port_ = *port;
        thread_ = std::thread(
            [this]
            {
                auto started = server_->start();
                EXPECT_TRUE(started) << (started ? "" : started.error().error_message);
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        a_ = connect();
        b_ = connect();
        ASSERT_NE(a_, nullptr);
        ASSERT_NE(b_, nullptr);
    }

    void TearDown() override
    {
        for (auto * c : {a_, b_})
        {
            if (c != nullptr)
            {
                mysql_close(c);
            }
        }
        a_ = b_ = nullptr;
        if (server_)
        {
            server_->stop();
        }
        if (thread_.joinable())
        {
            thread_.join();
        }
        // Let the detached session threads notice the closed sockets and exit
        // before the engine they reference is destroyed.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (engine_)
        {
            engine_->shutdown();
        }
        engine_.reset();
        server_.reset();
        schema_mgr_.reset();
        std::filesystem::remove_all(dir_);
    }

    MYSQL * connect()
    {
        MYSQL * c = mysql_init(nullptr);
        if (c == nullptr)
        {
            return nullptr;
        }
        if (mysql_real_connect(c, "127.0.0.1", "root", "", nullptr, port_, nullptr, 0) == nullptr)
        {
            ADD_FAILURE() << "PopoDB connect failed: " << mysql_error(c);
            mysql_close(c);
            return nullptr;
        }
        return c;
    }

    void must(MYSQL * c, const std::string & sql)
    {
        ASSERT_EQ(mysql_real_query(c, sql.c_str(), static_cast<unsigned long>(sql.size())), 0)
            << sql << " -> " << mysql_error(c);
        MYSQL_RES * res = mysql_store_result(c);
        if (res != nullptr)
        {
            mysql_free_result(res);
        }
    }

    // First column of every row, as the wire protocol renders it.
    std::vector<std::string> column(MYSQL * c, const std::string & sql)
    {
        std::vector<std::string> out;
        if (mysql_real_query(c, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
        {
            ADD_FAILURE() << sql << " -> " << mysql_error(c);
            return out;
        }
        MYSQL_RES * res = mysql_store_result(c);
        if (res == nullptr)
        {
            return out;
        }
        while (MYSQL_ROW row = mysql_fetch_row(res))
        {
            out.emplace_back(row[0] != nullptr ? row[0] : "NULL");
        }
        mysql_free_result(res);
        return out;
    }

    std::string one(MYSQL * c, const std::string & sql)
    {
        auto values = column(c, sql);
        EXPECT_EQ(values.size(), 1u) << sql;
        return values.empty() ? std::string{} : values.front();
    }

    // Empty when the statement unexpectedly succeeded, else the server's message.
    std::string error_of(MYSQL * c, const std::string & sql)
    {
        if (mysql_real_query(c, sql.c_str(), static_cast<unsigned long>(sql.size())) == 0)
        {
            MYSQL_RES * res = mysql_store_result(c);
            if (res != nullptr)
            {
                mysql_free_result(res);
            }
            return {};
        }
        return mysql_error(c);
    }

    void seed()
    {
        must(a_, "CREATE TABLE t (id INT PRIMARY KEY, v INT)");
        must(a_, "INSERT INTO t VALUES (1, 10)");
    }

    std::filesystem::path dir_;
    std::unique_ptr<popo::catalog::SchemaManager> schema_mgr_;
    std::unique_ptr<popo::storage::StorageEngine> engine_;
    std::unique_ptr<popo::server::Server> server_;
    std::thread thread_;
    uint16_t port_ = 0;
    MYSQL * a_ = nullptr;
    MYSQL * b_ = nullptr;
};

TEST_F(ServerIsolationTest, BeginCommitRollbackSpanStatements)
{
    seed();

    must(a_, "BEGIN");
    must(a_, "INSERT INTO t VALUES (2, 20)");
    EXPECT_EQ(one(a_, "SELECT COUNT(*) FROM t"), "2");
    EXPECT_EQ(one(b_, "SELECT COUNT(*) FROM t"), "1");
    must(a_, "ROLLBACK");
    EXPECT_EQ(one(a_, "SELECT COUNT(*) FROM t"), "1");

    must(a_, "START TRANSACTION");
    must(a_, "INSERT INTO t VALUES (3, 30)");
    must(a_, "COMMIT");
    EXPECT_EQ(one(b_, "SELECT COUNT(*) FROM t"), "2");
}

TEST_F(ServerIsolationTest, IsolationLevelScopesAndReadback)
{
    EXPECT_EQ(one(a_, "SELECT @@transaction_isolation"), "READ-COMMITTED");

    must(a_, "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    EXPECT_EQ(one(a_, "SELECT @@transaction_isolation"), "SERIALIZABLE");

    // Without a scope the level applies to the next transaction only.
    must(a_, "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED");
    must(a_, "BEGIN");
    EXPECT_EQ(one(a_, "SELECT @@transaction_isolation"), "READ-UNCOMMITTED");
    must(a_, "COMMIT");
    EXPECT_EQ(one(a_, "SELECT @@transaction_isolation"), "SERIALIZABLE");

    // The assignment form and the legacy variable name are both accepted.
    must(a_, "SET SESSION transaction_isolation = 'REPEATABLE-READ'");
    EXPECT_EQ(one(a_, "SELECT @@transaction_isolation"), "REPEATABLE-READ");

    // GLOBAL changes the default for sessions that have no override of their own.
    must(a_, "SET GLOBAL TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    EXPECT_EQ(one(b_, "SELECT @@transaction_isolation"), "SERIALIZABLE");
    EXPECT_EQ(one(a_, "SELECT @@transaction_isolation"), "REPEATABLE-READ");
}

TEST_F(ServerIsolationTest, IsolationCannotChangeMidTransaction)
{
    must(a_, "BEGIN");
    EXPECT_FALSE(error_of(a_, "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE").empty());
    must(a_, "COMMIT");
}

TEST_F(ServerIsolationTest, ReadCommittedSeesEachCommit)
{
    seed();
    must(a_, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
    must(a_, "BEGIN");
    EXPECT_EQ(one(a_, "SELECT v FROM t WHERE id = 1"), "10");

    must(b_, "UPDATE t SET v = 99 WHERE id = 1");
    EXPECT_EQ(one(a_, "SELECT v FROM t WHERE id = 1"), "99");
    must(a_, "COMMIT");
}

TEST_F(ServerIsolationTest, RepeatableReadKeepsOneSnapshot)
{
    seed();
    must(a_, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    must(a_, "BEGIN");
    EXPECT_EQ(one(a_, "SELECT v FROM t WHERE id = 1"), "10");

    must(b_, "UPDATE t SET v = 99 WHERE id = 1");
    // The point-select fast path has to honour the open transaction too.
    EXPECT_EQ(one(a_, "SELECT v FROM t WHERE id = 1"), "10");
    EXPECT_EQ(one(a_, "SELECT COUNT(*) FROM t"), "1");
    must(a_, "COMMIT");
    EXPECT_EQ(one(a_, "SELECT v FROM t WHERE id = 1"), "99");
}

TEST_F(ServerIsolationTest, ReadUncommittedSeesADirtyRow)
{
    seed();
    must(a_, "SET SESSION TRANSACTION ISOLATION LEVEL READ UNCOMMITTED");
    must(a_, "BEGIN");
    must(b_, "BEGIN");
    must(b_, "UPDATE t SET v = 99 WHERE id = 1");

    EXPECT_EQ(one(a_, "SELECT v FROM t WHERE id = 1"), "99");
    must(b_, "ROLLBACK");
    EXPECT_EQ(one(a_, "SELECT v FROM t WHERE id = 1"), "10");
    must(a_, "COMMIT");
}

TEST_F(ServerIsolationTest, RepeatableReadAbortsTheLoserOfAWriteConflict)
{
    seed();
    must(a_, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    must(b_, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    must(a_, "BEGIN");
    must(b_, "BEGIN");

    must(a_, "UPDATE t SET v = 100 WHERE id = 1");
    must(b_, "UPDATE t SET v = 200 WHERE id = 1");
    must(a_, "COMMIT");

    EXPECT_FALSE(error_of(b_, "COMMIT").empty()) << "the second committer must lose";
    EXPECT_EQ(one(a_, "SELECT v FROM t WHERE id = 1"), "100");
    // The aborted connection is still usable.
    EXPECT_EQ(one(b_, "SELECT v FROM t WHERE id = 1"), "100");
}

TEST_F(ServerIsolationTest, SerializableRejectsAPhantomInsert)
{
    seed();
    must(a_, "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    must(a_, "BEGIN");
    EXPECT_EQ(one(a_, "SELECT COUNT(*) FROM t"), "1");

    must(b_, "INSERT INTO t VALUES (2, 20)");
    EXPECT_FALSE(error_of(a_, "COMMIT").empty()) << "a phantom must abort the reader";
    EXPECT_EQ(one(a_, "SELECT COUNT(*) FROM t"), "2");
}

TEST_F(ServerIsolationTest, AutocommitOffGroupsStatements)
{
    seed();
    must(a_, "SET autocommit = 0");
    must(a_, "INSERT INTO t VALUES (2, 20)");

    EXPECT_EQ(one(a_, "SELECT COUNT(*) FROM t"), "2");
    EXPECT_EQ(one(b_, "SELECT COUNT(*) FROM t"), "1");

    must(a_, "COMMIT");
    EXPECT_EQ(one(b_, "SELECT COUNT(*) FROM t"), "2");

    must(a_, "SET autocommit = 1");
    must(a_, "INSERT INTO t VALUES (3, 30)");
    EXPECT_EQ(one(b_, "SELECT COUNT(*) FROM t"), "3");
}

TEST_F(ServerIsolationTest, RollbackDiscardsEverythingTheTransactionBuffered)
{
    seed();
    must(a_, "BEGIN");
    must(a_, "INSERT INTO t VALUES (2, 20)");
    must(a_, "UPDATE t SET v = 11 WHERE id = 1");
    EXPECT_EQ(one(a_, "SELECT v FROM t WHERE id = 1"), "11");
    must(a_, "ROLLBACK");

    EXPECT_EQ(one(a_, "SELECT v FROM t WHERE id = 1"), "10");
    EXPECT_EQ(one(b_, "SELECT COUNT(*) FROM t"), "1");
}

TEST_F(ServerIsolationTest, DuplicateKeyIsReportedAndLeavesNothingBehind)
{
    seed();
    must(a_, "BEGIN");
    must(a_, "INSERT INTO t VALUES (2, 20)");
    EXPECT_FALSE(error_of(a_, "INSERT INTO t VALUES (1, 99)").empty());
    // A failed statement rolls the whole transaction back, so the successful
    // insert before it is gone as well.
    EXPECT_EQ(one(b_, "SELECT COUNT(*) FROM t"), "1");
    EXPECT_EQ(one(a_, "SELECT COUNT(*) FROM t"), "1");
}

} // namespace
