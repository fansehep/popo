#include "sql_runner.h"

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "src/parser/ast.h"
#include "src/trancation/txn.h"

namespace
{

using popo::test::SqlSuccess;
using popo::txn::IsolationLevel;
using Txn = popo::txn::Transaction;

// Two logical connections over one engine. Isolation is a per-transaction
// property, so interleaving statements through exec_in() reproduces exactly what
// two client connections would see.
class IsolationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::random_device rd;
        dir_ = std::filesystem::temp_directory_path() / ("popo_isolation_test_" + std::to_string(rd()));
        std::filesystem::create_directories(dir_);
        runner_ = std::make_unique<popo::test::SqlRunner>(dir_.string());
    }

    void TearDown() override
    {
        runner_.reset();
        std::filesystem::remove_all(dir_);
    }

    SqlSuccess must(const std::string & sql)
    {
        auto r = runner_->exec(sql);
        EXPECT_TRUE(r) << sql << " -> " << (r ? "" : r.error().error_message);
        return r ? std::move(*r) : SqlSuccess{};
    }

    SqlSuccess must_in(const std::string & sql, Txn * txn)
    {
        auto r = runner_->exec_in(sql, txn);
        EXPECT_TRUE(r) << sql << " -> " << (r ? "" : r.error().error_message);
        return r ? std::move(*r) : SqlSuccess{};
    }

    // First column of one row. INT columns arrive as Int32 and COUNT(*) as
    // Int64, so widen through the numeric conversion rather than a typed getter.
    static int64_t nth(const SqlSuccess & r, size_t row)
    {
        EXPECT_LT(row, r.result.rows.size());
        return row < r.result.rows.size() ? static_cast<int64_t>(r.result.rows[row]->values_[0].to_f64()) : 0;
    }

    static int64_t one(const SqlSuccess & r)
    {
        EXPECT_EQ(r.result.rows.size(), 1u);
        return nth(r, 0);
    }

    void seed()
    {
        must("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
        must("INSERT INTO t VALUES (1, 10), (2, 20)");
    }

    Txn * open(IsolationLevel level) { return runner_->engine().begin_txn(level); }

    std::filesystem::path dir_;
    std::unique_ptr<popo::test::SqlRunner> runner_;
};

TEST_F(IsolationTest, ReadUncommittedSeesUncommittedWrites)
{
    seed();
    Txn * reader = open(IsolationLevel::ReadUncommitted);
    Txn * writer = open(IsolationLevel::ReadCommitted);

    must_in("UPDATE t SET v = 99 WHERE id = 1", writer);

    // Both the point-select fast path and a snapshot scan report the dirty row.
    EXPECT_EQ(one(must_in("SELECT v FROM t WHERE id = 1", reader)), 99);
    EXPECT_EQ(nth(must_in("SELECT v FROM t WHERE id > 0", reader), 0), 99);

    runner_->engine().abort_txn(writer);
    EXPECT_EQ(one(must_in("SELECT v FROM t WHERE id = 1", reader)), 10);
    runner_->engine().abort_txn(reader);
}

TEST_F(IsolationTest, ReadCommittedIgnoresUncommittedThenSeesTheCommit)
{
    seed();
    Txn * reader = open(IsolationLevel::ReadCommitted);
    Txn * writer = open(IsolationLevel::ReadCommitted);

    must_in("UPDATE t SET v = 99 WHERE id = 1", writer);
    EXPECT_EQ(one(must_in("SELECT v FROM t WHERE id = 1", reader)), 10);

    EXPECT_TRUE(runner_->engine().commit_txn(writer));
    // Non-repeatable read is permitted at this level: the next statement moves to
    // a fresh snapshot.
    EXPECT_EQ(one(must_in("SELECT v FROM t WHERE id = 1", reader)), 99);
    runner_->engine().abort_txn(reader);
}

TEST_F(IsolationTest, RepeatableReadKeepsOneSnapshot)
{
    seed();
    Txn * reader = open(IsolationLevel::RepeatableRead);
    EXPECT_EQ(one(must_in("SELECT v FROM t WHERE id = 1", reader)), 10);

    Txn * writer = open(IsolationLevel::ReadCommitted);
    must_in("UPDATE t SET v = 99 WHERE id = 1", writer);
    EXPECT_TRUE(runner_->engine().commit_txn(writer));

    EXPECT_EQ(one(must_in("SELECT v FROM t WHERE id = 1", reader)), 10);
    EXPECT_EQ(one(must_in("SELECT COUNT(*) FROM t", reader)), 2);
    runner_->engine().abort_txn(reader);

    // The committed value is still there for everybody else.
    EXPECT_EQ(one(must("SELECT v FROM t WHERE id = 1")), 99);
}

TEST_F(IsolationTest, RepeatableReadAbortsTheLoserOfAWriteConflict)
{
    seed();
    Txn * first = open(IsolationLevel::RepeatableRead);
    Txn * second = open(IsolationLevel::RepeatableRead);

    must_in("UPDATE t SET v = 100 WHERE id = 1", first);
    must_in("UPDATE t SET v = 200 WHERE id = 1", second);

    ASSERT_TRUE(runner_->engine().commit_txn(first));
    // First-committer-wins: second wrote a key that changed after its snapshot.
    auto committed = runner_->engine().commit_txn(second);
    ASSERT_FALSE(committed);
    EXPECT_EQ(committed.error().error_code, static_cast<int32_t>(popo::base::Errno::SerializationFailure));

    EXPECT_EQ(one(must("SELECT v FROM t WHERE id = 1")), 100);
}

TEST_F(IsolationTest, ReadCommittedLastWriterWins)
{
    seed();
    Txn * first = open(IsolationLevel::ReadCommitted);
    Txn * second = open(IsolationLevel::ReadCommitted);

    must_in("UPDATE t SET v = 100 WHERE id = 1", first);
    must_in("UPDATE t SET v = 200 WHERE id = 1", second);

    EXPECT_TRUE(runner_->engine().commit_txn(first));
    EXPECT_TRUE(runner_->engine().commit_txn(second));
    EXPECT_EQ(one(must("SELECT v FROM t WHERE id = 1")), 200);
}

TEST_F(IsolationTest, SerializableRejectsAPhantomInsert)
{
    seed();
    Txn * reader = open(IsolationLevel::Serializable);
    EXPECT_EQ(one(must_in("SELECT COUNT(*) FROM t", reader)), 2);

    Txn * writer = open(IsolationLevel::ReadCommitted);
    must_in("INSERT INTO t VALUES (3, 30)", writer);
    EXPECT_TRUE(runner_->engine().commit_txn(writer));

    auto committed = runner_->engine().commit_txn(reader);
    ASSERT_FALSE(committed);
    EXPECT_EQ(committed.error().error_code, static_cast<int32_t>(popo::base::Errno::SerializationFailure));
    EXPECT_EQ(one(must("SELECT COUNT(*) FROM t")), 3);
}

TEST_F(IsolationTest, SerializableAcceptsAnUnconflictedTransaction)
{
    seed();
    Txn * txn = open(IsolationLevel::Serializable);
    EXPECT_EQ(one(must_in("SELECT v FROM t WHERE id = 1", txn)), 10);
    must_in("INSERT INTO t VALUES (3, 30)", txn);
    EXPECT_TRUE(runner_->engine().commit_txn(txn));
    EXPECT_EQ(one(must("SELECT COUNT(*) FROM t")), 3);
}

TEST_F(IsolationTest, RollbackDiscardsBufferedWrites)
{
    seed();
    Txn * txn = open(IsolationLevel::RepeatableRead);
    must_in("INSERT INTO t VALUES (9, 90)", txn);
    must_in("UPDATE t SET v = 11 WHERE id = 1", txn);

    // A transaction always sees its own buffered writes.
    EXPECT_EQ(one(must_in("SELECT v FROM t WHERE id = 9", txn)), 90);
    EXPECT_EQ(one(must_in("SELECT v FROM t WHERE id = 1", txn)), 11);

    runner_->engine().abort_txn(txn);

    EXPECT_TRUE(must("SELECT v FROM t WHERE id = 9").result.rows.empty());
    EXPECT_EQ(one(must("SELECT v FROM t WHERE id = 1")), 10);
    EXPECT_EQ(one(must("SELECT COUNT(*) FROM t")), 2);
}

TEST_F(IsolationTest, DuplicatePrimaryKeyIsRejectedWhenBuffered)
{
    seed();
    Txn * txn = open(IsolationLevel::ReadCommitted);
    auto dup = runner_->exec_in("INSERT INTO t VALUES (1, 99)", txn);
    ASSERT_FALSE(dup);
    EXPECT_NE(dup.error().error_message.find("Duplicate entry"), std::string::npos) << dup.error().error_message;
    EXPECT_EQ(one(must_in("SELECT v FROM t WHERE id = 1", txn)), 10);

    // Deleting the row in the same transaction frees the key again.
    must_in("DELETE FROM t WHERE id = 1", txn);
    must_in("INSERT INTO t VALUES (1, 77)", txn);
    EXPECT_TRUE(runner_->engine().commit_txn(txn));
    EXPECT_EQ(one(must("SELECT v FROM t WHERE id = 1")), 77);
}

TEST_F(IsolationTest, UniqueSecondaryKeyIsCheckedWhenTheWriteIsBuffered)
{
    must("CREATE TABLE u (id INT PRIMARY KEY, email VARCHAR(32))");
    must("CREATE UNIQUE INDEX u_email ON u (email)");
    must("INSERT INTO u VALUES (1, 'a@x')");

    Txn * txn = open(IsolationLevel::ReadCommitted);
    auto dup = runner_->exec_in("INSERT INTO u VALUES (2, 'a@x')", txn);
    ASSERT_FALSE(dup);
    EXPECT_NE(dup.error().error_message.find("Duplicate entry"), std::string::npos) << dup.error().error_message;

    // The rejected write must not have been buffered.
    EXPECT_EQ(one(must_in("SELECT COUNT(*) FROM u", txn)), 1);
    must_in("INSERT INTO u VALUES (2, 'b@x')", txn);
    EXPECT_TRUE(runner_->engine().commit_txn(txn));
    EXPECT_EQ(one(must("SELECT COUNT(*) FROM u")), 2);
}

TEST_F(IsolationTest, BufferedDeleteFreesAUniqueKey)
{
    must("CREATE TABLE u (id INT PRIMARY KEY, email VARCHAR(32))");
    must("CREATE UNIQUE INDEX u_email ON u (email)");
    must("INSERT INTO u VALUES (1, 'a@x')");

    Txn * txn = open(IsolationLevel::ReadCommitted);
    must_in("DELETE FROM u WHERE id = 1", txn);
    must_in("INSERT INTO u VALUES (2, 'a@x')", txn);
    EXPECT_TRUE(runner_->engine().commit_txn(txn));

    auto r = must("SELECT id FROM u WHERE id > 0");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(one(r), 2);
}

// ---------------------------------------------------------------------------
// Session-level state: what the server Session drives per connection.
// ---------------------------------------------------------------------------

TEST_F(IsolationTest, SessionReportsTheConfiguredIsolationLevel)
{
    auto r = must("SELECT @@transaction_isolation");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "READ-COMMITTED");

    EXPECT_TRUE(runner_->session().set_session_isolation(popo::parser::IsolationLevel::Serializable));
    r = must("SELECT @@transaction_isolation");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "SERIALIZABLE");

    EXPECT_TRUE(runner_->session().set_session_isolation(popo::parser::IsolationLevel::RepeatableRead));
    auto statement_txn = runner_->session().statement_txn();
    ASSERT_TRUE(statement_txn);
    auto * txn = *statement_txn;
    EXPECT_EQ(txn->isolation(), IsolationLevel::RepeatableRead);
    EXPECT_EQ(runner_->session().current()->isolation(), IsolationLevel::RepeatableRead);
    runner_->session().rollback();
}

TEST_F(IsolationTest, SessionNextTransactionLevelAppliesOnce)
{
    auto & session = runner_->session();
    EXPECT_TRUE(session.set_session_isolation(popo::parser::IsolationLevel::ReadCommitted));
    session.set_next_isolation(popo::parser::IsolationLevel::Serializable);

    ASSERT_TRUE(session.begin());
    EXPECT_EQ(session.current()->isolation(), IsolationLevel::Serializable);
    session.rollback();

    ASSERT_TRUE(session.begin());
    EXPECT_EQ(session.current()->isolation(), IsolationLevel::ReadCommitted);
    session.rollback();
}

TEST_F(IsolationTest, SessionIsolationAppliesToTheRunningTransaction)
{
    seed();
    auto & session = runner_->session();
    ASSERT_TRUE(session.set_session_isolation(popo::parser::IsolationLevel::RepeatableRead));
    ASSERT_TRUE(session.begin());
    ASSERT_TRUE(session.in_transaction());
    EXPECT_EQ(session.current()->isolation(), IsolationLevel::RepeatableRead);

    EXPECT_EQ(nth(must("SELECT v FROM t WHERE id > 0"), 0), 10);

    Txn * writer = open(IsolationLevel::ReadCommitted);
    must_in("UPDATE t SET v = 99 WHERE id = 1", writer);
    EXPECT_TRUE(runner_->engine().commit_txn(writer));

    // Still inside the transaction opened above, so the snapshot still holds.
    EXPECT_EQ(one(must("SELECT v FROM t WHERE id = 1")), 10);
    session.rollback();
    EXPECT_EQ(one(must("SELECT v FROM t WHERE id = 1")), 99);
}

TEST_F(IsolationTest, AutocommitOffKeepsOneTransactionAcrossStatements)
{
    seed();
    auto & session = runner_->session();
    ASSERT_TRUE(session.set_autocommit(false));

    must("INSERT INTO t VALUES (3, 30)");
    EXPECT_TRUE(session.in_transaction());
    EXPECT_EQ(one(must("SELECT COUNT(*) FROM t")), 3);

    session.rollback();
    EXPECT_EQ(one(must("SELECT COUNT(*) FROM t")), 2);

    ASSERT_TRUE(session.set_autocommit(true));
    must("INSERT INTO t VALUES (3, 30)");
    EXPECT_FALSE(session.in_transaction());
    EXPECT_EQ(one(must("SELECT COUNT(*) FROM t")), 3);
}

TEST_F(IsolationTest, FailedStatementPoisonsTheTransaction)
{
    seed();
    auto & session = runner_->session();
    ASSERT_TRUE(session.set_autocommit(false));

    must("INSERT INTO t VALUES (3, 30)");
    // Duplicate primary key: the statement fails, so its partial buffered write
    // and the successful one before it must both be gone.
    auto dup = runner_->exec("INSERT INTO t VALUES (3, 99)");
    EXPECT_FALSE(dup);
    EXPECT_FALSE(session.in_transaction());
    EXPECT_EQ(one(must("SELECT COUNT(*) FROM t")), 2);

    EXPECT_TRUE(session.set_autocommit(true));
}

} // namespace
