#include <sqlite3.h>

#include "base/error.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using popo::base::Errno;
using popo::base::Result;

#define BENCH_CONCAT_INNER(a, b) a##b
#define BENCH_CONCAT(a, b) BENCH_CONCAT_INNER(a, b)
#define BENCH_TRY(expr) BENCH_TRY_IMPL(expr, __LINE__)
#define BENCH_TRY_IMPL(expr, line)                                        \
    do                                                                    \
    {                                                                     \
        auto BENCH_CONCAT(result_, line) = (expr);                        \
        if (!BENCH_CONCAT(result_, line))                                 \
            return std::unexpected(BENCH_CONCAT(result_, line).error());  \
    } while (false)
#define BENCH_TRY_ASSIGN(name, expr) BENCH_TRY_ASSIGN_IMPL(name, expr, __LINE__)
#define BENCH_TRY_ASSIGN_IMPL(name, expr, line)                            \
    auto BENCH_CONCAT(result_, line) = (expr);                            \
    if (!BENCH_CONCAT(result_, line))                                     \
        return std::unexpected(BENCH_CONCAT(result_, line).error());      \
    auto name = std::move(*BENCH_CONCAT(result_, line))

constexpr const char * kDataRoot = "/home/sehep/github/neon/data/sqlite-oltp";

struct Sample
{
    const char * suite;
    const char * workload;
    int64_t ops = 0;
    double ms = 0;
};

std::vector<Sample> g_samples;
std::mt19937 g_rng(1);

int pick(int n)
{
    return std::uniform_int_distribution<int>(1, n)(g_rng);
}

double elapsed_ms(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void record(const char * suite, const char * workload, int64_t ops, std::chrono::steady_clock::time_point t0)
{
    Sample s;
    s.suite = suite;
    s.workload = workload;
    s.ops = ops;
    s.ms = elapsed_ms(t0);
    double tps = s.ms > 0 ? (s.ops * 1000.0 / s.ms) : 0;
    std::cout << "  " << suite << " " << workload << "  ops=" << ops << "  ms=" << s.ms << "  tps=" << tps << "\n";
    std::cout.flush();
    g_samples.push_back(s);
}

struct ExecuteSuccess
{
    struct Row
    {
        std::vector<int64_t> values_;
    };
    int64_t rows_affected = 0;
    std::vector<std::shared_ptr<Row>> rows;
};

class SqlRunner;

class SqlTxn
{
public:
    SqlTxn() = default;
    SqlTxn(SqlTxn && other) noexcept
        : runner_(other.runner_)
        , open_(other.open_)
    {
        other.runner_ = nullptr;
        other.open_ = false;
    }
    SqlTxn & operator=(SqlTxn &&) = delete;
    SqlTxn(const SqlTxn &) = delete;
    ~SqlTxn();

    Result<ExecuteSuccess> exec(const std::string & sql);
    Result<void> commit();

private:
    friend class SqlRunner;
    explicit SqlTxn(SqlRunner * runner)
        : runner_(runner)
        , open_(true)
    {
    }

    SqlRunner * runner_ = nullptr;
    bool open_ = false;
};

class SqlRunner
{
public:
    SqlRunner(SqlRunner && other) noexcept : db_(std::exchange(other.db_, nullptr)) {}
    SqlRunner(const SqlRunner &) = delete;
    SqlRunner & operator=(const SqlRunner &) = delete;
    SqlRunner & operator=(SqlRunner &&) = delete;

    static Result<SqlRunner> open(const std::string & dir)
    {
        std::filesystem::path path = std::filesystem::path(dir) / "bench.db";
        sqlite3 * db = nullptr;
        int rc = sqlite3_open(path.c_str(), &db);
        if (rc != SQLITE_OK)
        {
            std::string message = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
            if (db != nullptr)
                sqlite3_close(db);
            return popo::base::make_error(rc, "sqlite open: " + message);
        }
        SqlRunner out(db);
        BENCH_TRY(out.exec("PRAGMA journal_mode=DELETE"));
        BENCH_TRY(out.exec("PRAGMA synchronous=FULL"));
        BENCH_TRY(out.exec("PRAGMA temp_store=MEMORY"));
        BENCH_TRY(out.exec("PRAGMA cache_size=-65536"));
        return out;
    }

    ~SqlRunner()
    {
        if (db_ != nullptr)
        {
            sqlite3_close(db_);
        }
    }

    Result<ExecuteSuccess> exec(const std::string & sql) { return run(sql); }

    Result<SqlTxn> begin()
    {
        BENCH_TRY(exec("BEGIN IMMEDIATE"));
        return SqlTxn(this);
    }

private:
    friend class SqlTxn;

    Result<ExecuteSuccess> run(const std::string & sql)
    {
        ExecuteSuccess out;
        const char * tail = sql.c_str();
        while (tail != nullptr && *tail != '\0')
        {
            sqlite3_stmt * stmt = nullptr;
            int rc = sqlite3_prepare_v2(db_, tail, -1, &stmt, &tail);
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(stmt, sqlite3_finalize);
            if (rc != SQLITE_OK)
            {
                return popo::base::make_error(rc, sql + "\n" + sqlite3_errmsg(db_));
            }
            if (stmt == nullptr)
            {
                break;
            }
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                auto row = std::make_shared<ExecuteSuccess::Row>();
                int cols = sqlite3_column_count(stmt);
                row->values_.resize(static_cast<size_t>(cols));
                for (int i = 0; i < cols; ++i)
                {
                    row->values_[static_cast<size_t>(i)] = sqlite3_column_int64(stmt, i);
                }
                out.rows.push_back(std::move(row));
            }
            if (rc != SQLITE_DONE)
            {
                return popo::base::make_error(rc, sql + "\n" + sqlite3_errmsg(db_));
            }
            out.rows_affected = sqlite3_changes(db_);
        }
        return out;
    }

    explicit SqlRunner(sqlite3 * db) : db_(db) {}
    sqlite3 * db_ = nullptr;
};

SqlTxn::~SqlTxn()
{
    if (open_ && runner_ != nullptr)
    {
        (void)runner_->run("ROLLBACK");
        open_ = false;
    }
}

Result<ExecuteSuccess> SqlTxn::exec(const std::string & sql)
{
    if (!open_ || runner_ == nullptr)
    {
        return popo::base::make_error(Errno::InternalErr, "transaction is closed");
    }
    auto result = runner_->run(sql);
    if (!result)
    {
        (void)runner_->run("ROLLBACK");
        open_ = false;
        return std::unexpected(result.error());
    }
    return result;
}

Result<void> SqlTxn::commit()
{
    if (!open_ || runner_ == nullptr)
    {
        return popo::base::make_error(Errno::InternalErr, "transaction is closed");
    }
    BENCH_TRY(runner_->run("COMMIT"));
    open_ = false;
    return {};
}

Result<ExecuteSuccess> must(SqlRunner & db, const std::string & sql)
{
    return db.exec(sql);
}

Result<ExecuteSuccess> must(SqlTxn & txn, const std::string & sql)
{
    return txn.exec(sql);
}

Result<void> expect_write(Result<ExecuteSuccess> result, const std::string & sql)
{
    if (!result)
        return std::unexpected(result.error());
    if (result->rows_affected < 1)
    {
        return popo::base::make_error(Errno::InternalErr, "no row written: " + sql);
    }
    return {};
}

Result<int64_t> cell(Result<ExecuteSuccess> result, size_t col = 0)
{
    if (!result)
        return std::unexpected(result.error());
    if (result->rows.empty() || col >= result->rows[0]->values_.size())
    {
        return popo::base::make_error(Errno::InternalErr, "empty result");
    }
    return result->rows[0]->values_[col];
}

std::string q(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    out.append(s);
    out.push_back('\'');
    return out;
}

std::string field(int width, int salt)
{
    std::string s(static_cast<size_t>(width), static_cast<char>('a' + (salt % 26)));
    std::string tag = std::to_string(salt);
    if (tag.size() < s.size())
    {
        s.replace(0, tag.size(), tag);
    }
    return s;
}

Result<void> load_values(SqlRunner & db, const std::string & prefix, const std::vector<std::string> & tuples, int batch)
{
    for (size_t i = 0; i < tuples.size();)
    {
        std::string sql = prefix;
        sql.reserve(prefix.size() + static_cast<size_t>(batch) * 64);
        int n = 0;
        for (; i < tuples.size() && n < batch; ++i, ++n)
        {
            if (n != 0)
            {
                sql.push_back(',');
            }
            sql += tuples[i];
        }
        BENCH_TRY_ASSIGN(result, must(db, sql));
        if (result.rows_affected != n)
        {
            return popo::base::make_error(
                Errno::InternalErr,
                "insert batch wrote " + std::to_string(result.rows_affected) + " rows, expected " + std::to_string(n));
        }
    }
    return {};
}

Result<std::string> prepare_dir(const std::string & name)
{
    std::filesystem::path dir = std::filesystem::path(kDataRoot) / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec)
        return popo::base::make_error(Errno::IoErr, "remove " + dir.string() + ": " + ec.message());
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return popo::base::make_error(Errno::IoErr, "create " + dir.string() + ": " + ec.message());
    return dir.string();
}

Result<std::string> mount_of(const std::string & path)
{
    std::ifstream in("/proc/mounts");
    if (!in)
        return popo::base::make_error(Errno::IoErr, "cannot open /proc/mounts");
    std::string dev;
    std::string point;
    std::string fs;
    std::string best_dev;
    std::string best_point;
    std::string best_fs;
    while (in >> dev >> point >> fs)
    {
        std::string rest;
        std::getline(in, rest);
        if (path.compare(0, point.size(), point) == 0 && point.size() >= best_point.size())
        {
            best_dev = dev;
            best_point = point;
            best_fs = fs;
        }
    }
    if (best_fs.empty())
    {
        return popo::base::make_error(Errno::IoErr, "cannot resolve filesystem for " + path);
    }
    if (best_fs == "tmpfs")
    {
        return popo::base::make_error(Errno::InvalidArg, "refusing tmpfs at " + best_point);
    }
    return best_dev + " " + best_fs + " at " + best_point;
}

Result<uint64_t> dir_bytes(const std::filesystem::path & root)
{
    uint64_t n = 0;
    std::error_code ec;
    auto it = std::filesystem::recursive_directory_iterator(root, ec);
    if (ec)
        return popo::base::make_error(Errno::IoErr, "scan " + root.string() + ": " + ec.message());
    for (; it != std::filesystem::recursive_directory_iterator();)
    {
        if (it->is_regular_file(ec))
        {
            n += it->file_size(ec);
        }
        if (ec)
            return popo::base::make_error(Errno::IoErr, "inspect " + it->path().string() + ": " + ec.message());
        it.increment(ec);
        if (ec)
            return popo::base::make_error(Errno::IoErr, "scan " + root.string() + ": " + ec.message());
    }
    return n;
}

Result<void> run_sysbench()
{
    constexpr int kRows = 2000;
    constexpr int kOps = 200;
    std::cout << ">> sysbench\n";
    BENCH_TRY_ASSIGN(dir, prepare_dir("sysbench"));
    BENCH_TRY_ASSIGN(db, SqlRunner::open(dir));
    BENCH_TRY(must(db, "CREATE TABLE sbtest1 (id INT PRIMARY KEY, k INT, c VARCHAR(120), pad VARCHAR(60))"));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> tuples;
    tuples.reserve(kRows);
    for (int id = 1; id <= kRows; ++id)
    {
        tuples.push_back("(" + std::to_string(id) + "," + std::to_string(id % 1000) + "," + q(field(120, id)) + "," + q(field(60, id + 7)) + ")");
    }
    BENCH_TRY(load_values(db, "INSERT INTO sbtest1 VALUES ", tuples, 50));
    record("sysbench", "load", kRows, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        BENCH_TRY_ASSIGN(result, must(db, "SELECT c FROM sbtest1 WHERE id = " + std::to_string(id)));
        if (result.rows.size() != 1)
        {
            return popo::base::make_error(Errno::InternalErr, "sysbench point select missed id");
        }
    }
    record("sysbench", "oltp_point_select", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        std::string sql = "UPDATE sbtest1 SET k = k + 1 WHERE id = " + std::to_string(pick(kRows));
        BENCH_TRY(expect_write(must(db, sql), sql));
    }
    record("sysbench", "oltp_update_index", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        std::string sql = "UPDATE sbtest1 SET c = " + q(field(120, 100000 + i)) + " WHERE id = " + std::to_string(id);
        BENCH_TRY(expect_write(must(db, sql), sql));
    }
    record("sysbench", "oltp_update_non_index", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = kRows + 1 + i;
        std::string sql = "INSERT INTO sbtest1 VALUES (" + std::to_string(id) + "," + std::to_string(i) + "," + q(field(120, id)) + ","
            + q(field(60, id)) + ")";
        BENCH_TRY(expect_write(must(db, sql), sql));
    }
    record("sysbench", "oltp_insert", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = kRows + 1 + i;
        std::string sql = "DELETE FROM sbtest1 WHERE id = " + std::to_string(id);
        BENCH_TRY(expect_write(must(db, sql), sql));
    }
    record("sysbench", "oltp_delete", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    constexpr int kRange = 40;
    for (int i = 0; i < kRange; ++i)
    {
        int lo = pick(kRows - 20);
        BENCH_TRY(must(db, "SELECT c FROM sbtest1 WHERE id >= " + std::to_string(lo) + " AND id <= " + std::to_string(lo + 10)));
    }
    record("sysbench", "oltp_range_select", kRange, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        BENCH_TRY_ASSIGN(txn, db.begin());
        for (int s = 0; s < 10; ++s)
        {
            BENCH_TRY(must(txn, "SELECT c FROM sbtest1 WHERE id = " + std::to_string(pick(kRows))));
        }
        BENCH_TRY(must(txn, "SELECT c FROM sbtest1 WHERE id >= " + std::to_string(id) + " AND id <= " + std::to_string(id + 5)));
        BENCH_TRY(expect_write(must(txn, "UPDATE sbtest1 SET k = k + 1 WHERE id = " + std::to_string(id)), "rw update k"));
        BENCH_TRY(expect_write(must(txn, "UPDATE sbtest1 SET c = " + q(field(120, 200000 + i)) + " WHERE id = " + std::to_string(id)), "rw update c"));
        int fresh = kRows + 10000 + i;
        BENCH_TRY(expect_write(
            must(txn, "INSERT INTO sbtest1 VALUES (" + std::to_string(fresh) + ",1," + q(field(120, fresh)) + "," + q(field(60, fresh)) + ")"),
            "rw insert"));
        BENCH_TRY(expect_write(must(txn, "DELETE FROM sbtest1 WHERE id = " + std::to_string(fresh)), "rw delete"));
        BENCH_TRY(txn.commit());
    }
    record("sysbench", "oltp_read_write", kOps, t0);
    return {};
}

Result<void> run_pgbench()
{
    constexpr int kAccounts = 2000;
    constexpr int kTellers = 10;
    constexpr int kTxns = 200;
    std::cout << ">> pgbench\n";
    BENCH_TRY_ASSIGN(dir, prepare_dir("pgbench"));
    BENCH_TRY_ASSIGN(db, SqlRunner::open(dir));
    BENCH_TRY(must(db, "CREATE TABLE pgbench_branches (bid INT PRIMARY KEY, bbalance INT)"));
    BENCH_TRY(must(db, "CREATE TABLE pgbench_tellers (tid INT PRIMARY KEY, bid INT, tbalance INT)"));
    BENCH_TRY(must(db, "CREATE TABLE pgbench_accounts (aid INT PRIMARY KEY, bid INT, abalance INT)"));
    BENCH_TRY(must(db, "CREATE TABLE pgbench_history (hid INT PRIMARY KEY, tid INT, bid INT, aid INT, delta INT)"));

    auto t0 = std::chrono::steady_clock::now();
    BENCH_TRY(must(db, "INSERT INTO pgbench_branches VALUES (1,0)"));
    std::vector<std::string> tellers;
    for (int tid = 1; tid <= kTellers; ++tid)
    {
        tellers.push_back("(" + std::to_string(tid) + ",1,0)");
    }
    BENCH_TRY(load_values(db, "INSERT INTO pgbench_tellers VALUES ", tellers, 10));
    std::vector<std::string> accounts;
    accounts.reserve(kAccounts);
    for (int aid = 1; aid <= kAccounts; ++aid)
    {
        accounts.push_back("(" + std::to_string(aid) + ",1,0)");
    }
    BENCH_TRY(load_values(db, "INSERT INTO pgbench_accounts VALUES ", accounts, 100));
    record("pgbench", "load", kAccounts, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kTxns; ++i)
    {
        int aid = pick(kAccounts);
        int tid = pick(kTellers);
        int delta = (i % 2 == 0) ? 1 : -1;
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY(expect_write(must(txn, "UPDATE pgbench_accounts SET abalance = abalance + " + std::to_string(delta) + " WHERE aid = " + std::to_string(aid)), "account"));
        BENCH_TRY(must(txn, "SELECT abalance FROM pgbench_accounts WHERE aid = " + std::to_string(aid)));
        BENCH_TRY(expect_write(must(txn, "UPDATE pgbench_tellers SET tbalance = tbalance + " + std::to_string(delta) + " WHERE tid = " + std::to_string(tid)), "teller"));
        BENCH_TRY(expect_write(must(txn, "UPDATE pgbench_branches SET bbalance = bbalance + " + std::to_string(delta) + " WHERE bid = 1"), "branch"));
        BENCH_TRY(expect_write(
            must(txn, "INSERT INTO pgbench_history VALUES (" + std::to_string(i + 1) + "," + std::to_string(tid) + ",1," + std::to_string(aid) + "," + std::to_string(delta) + ")"),
            "history"));
        BENCH_TRY(txn.commit());
    }
    record("pgbench", "tpcb", kTxns, t0);
    return {};
}

Result<void> run_ycsb()
{
    constexpr int kRows = 1000;
    constexpr int kOps = 200;
    constexpr int kFields = 10;
    constexpr int kWidth = 100;
    std::cout << ">> ycsb\n";
    BENCH_TRY_ASSIGN(dir, prepare_dir("ycsb"));
    BENCH_TRY_ASSIGN(db, SqlRunner::open(dir));
    std::string ddl = "CREATE TABLE usertable (id INT PRIMARY KEY";
    for (int f = 0; f < kFields; ++f)
    {
        ddl += ", f" + std::to_string(f) + " VARCHAR(" + std::to_string(kWidth) + ")";
    }
    ddl += ")";
    BENCH_TRY(must(db, ddl));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> tuples;
    tuples.reserve(kRows);
    for (int id = 1; id <= kRows; ++id)
    {
        std::string row = "(" + std::to_string(id);
        for (int f = 0; f < kFields; ++f)
        {
            row += "," + q(field(kWidth, id * 17 + f));
        }
        row += ")";
        tuples.push_back(std::move(row));
    }
    BENCH_TRY(load_values(db, "INSERT INTO usertable VALUES ", tuples, 20));
    record("ycsb", "load", kRows, t0);

    auto mix = [&](const char * name, int reads_per_100) -> Result<void>
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i)
        {
            int id = pick(kRows);
            bool read = (i % 100) < reads_per_100;
            if (read)
            {
                BENCH_TRY_ASSIGN(result, must(db, "SELECT f0 FROM usertable WHERE id = " + std::to_string(id)));
                if (result.rows.size() != 1)
                {
                    return popo::base::make_error(Errno::InternalErr, "ycsb read missed");
                }
            }
            else
            {
                std::string sql = "UPDATE usertable SET f0 = " + q(field(kWidth, 500000 + i)) + " WHERE id = " + std::to_string(id);
                BENCH_TRY(expect_write(must(db, sql), sql));
            }
        }
        record("ycsb", name, kOps, start);
        return {};
    };
    BENCH_TRY(mix("workload_a", 50));
    BENCH_TRY(mix("workload_b", 95));
    BENCH_TRY(mix("workload_c", 100));

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY_ASSIGN(result, must(txn, "SELECT f0 FROM usertable WHERE id = " + std::to_string(id)));
        if (result.rows.size() != 1)
        {
            return popo::base::make_error(Errno::InternalErr, "ycsb rmw missed");
        }
        BENCH_TRY(expect_write(must(txn, "UPDATE usertable SET f0 = " + q(field(kWidth, 800000 + i)) + " WHERE id = " + std::to_string(id)), "ycsb rmw"));
        BENCH_TRY(txn.commit());
    }
    record("ycsb", "workload_f", kOps, start);
    return {};
}

Result<void> run_tpcc()
{
    constexpr int kDistricts = 4;
    constexpr int kCustomersPer = 50;
    constexpr int kItems = 200;
    constexpr int kNewOrder = 30;
    constexpr int kPayment = 30;
    constexpr int kCustomers = kDistricts * kCustomersPer;
    std::cout << ">> tpcc\n";
    BENCH_TRY_ASSIGN(dir, prepare_dir("tpcc"));
    BENCH_TRY_ASSIGN(db, SqlRunner::open(dir));
    BENCH_TRY(must(db, "CREATE TABLE warehouse (w_id INT PRIMARY KEY, w_ytd INT, w_name VARCHAR(16))"));
    BENCH_TRY(must(db, "CREATE TABLE district (d_id INT PRIMARY KEY, d_ytd INT, d_next_o_id INT, d_name VARCHAR(16))"));
    BENCH_TRY(must(db, "CREATE TABLE customer (c_id INT PRIMARY KEY, c_d_id INT, c_balance INT, c_last VARCHAR(16))"));
    BENCH_TRY(must(db, "CREATE TABLE item (i_id INT PRIMARY KEY, i_price INT, i_name VARCHAR(24))"));
    BENCH_TRY(must(db, "CREATE TABLE stock (s_id INT PRIMARY KEY, s_quantity INT, s_order_cnt INT)"));
    BENCH_TRY(must(db, "CREATE TABLE orders (o_id INT PRIMARY KEY, o_d_id INT, o_c_id INT, o_ol_cnt INT)"));
    BENCH_TRY(must(db, "CREATE TABLE new_order (no_id INT PRIMARY KEY, no_d_id INT, no_c_id INT)"));
    BENCH_TRY(must(db, "CREATE TABLE order_line (ol_id INT PRIMARY KEY, ol_o_id INT, ol_i_id INT, ol_amount INT)"));
    BENCH_TRY(must(db, "CREATE INDEX ol_o_id_idx ON order_line (ol_o_id)"));
    BENCH_TRY(must(db, "CREATE TABLE history (h_id INT PRIMARY KEY, h_c_id INT, h_d_id INT, h_amount INT)"));

    auto t0 = std::chrono::steady_clock::now();
    BENCH_TRY(must(db, "INSERT INTO warehouse VALUES (1,0,'w1')"));
    std::vector<std::string> rows;
    for (int d = 1; d <= kDistricts; ++d)
    {
        rows.push_back("(" + std::to_string(d) + ",0,1," + q("d" + std::to_string(d)) + ")");
    }
    BENCH_TRY(load_values(db, "INSERT INTO district VALUES ", rows, 10));
    rows.clear();
    for (int c = 1; c <= kCustomers; ++c)
    {
        int d = ((c - 1) / kCustomersPer) + 1;
        rows.push_back("(" + std::to_string(c) + "," + std::to_string(d) + ",1000," + q("c" + std::to_string(c)) + ")");
    }
    BENCH_TRY(load_values(db, "INSERT INTO customer VALUES ", rows, 50));
    rows.clear();
    for (int i = 1; i <= kItems; ++i)
    {
        rows.push_back("(" + std::to_string(i) + "," + std::to_string(10 + (i % 50)) + "," + q("item" + std::to_string(i)) + ")");
    }
    BENCH_TRY(load_values(db, "INSERT INTO item VALUES ", rows, 50));
    rows.clear();
    for (int i = 1; i <= kItems; ++i)
    {
        rows.push_back("(" + std::to_string(i) + ",50,0)");
    }
    BENCH_TRY(load_values(db, "INSERT INTO stock VALUES ", rows, 50));
    record("tpcc", "load", kCustomers + kItems, t0);

    std::vector<int> live_orders;
    int next_line = 1;
    int next_hist = 1;

    t0 = std::chrono::steady_clock::now();
    for (int n = 0; n < kNewOrder; ++n)
    {
        int d = pick(kDistricts);
        int c = (d - 1) * kCustomersPer + pick(kCustomersPer);
        constexpr int kLines = 5;
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY(must(txn, "SELECT w_ytd FROM warehouse WHERE w_id = 1"));
        BENCH_TRY_ASSIGN(seq, cell(must(txn, "SELECT d_next_o_id FROM district WHERE d_id = " + std::to_string(d))));
        int64_t oid = seq * 100 + d;
        BENCH_TRY(expect_write(must(txn, "UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = " + std::to_string(d)), "district"));
        BENCH_TRY(expect_write(
            must(txn, "INSERT INTO orders VALUES (" + std::to_string(oid) + "," + std::to_string(d) + "," + std::to_string(c) + "," + std::to_string(kLines) + ")"),
            "orders"));
        BENCH_TRY(expect_write(must(txn, "INSERT INTO new_order VALUES (" + std::to_string(oid) + "," + std::to_string(d) + "," + std::to_string(c) + ")"), "new_order"));
        for (int line = 0; line < kLines; ++line)
        {
            int item = pick(kItems);
            BENCH_TRY_ASSIGN(price, cell(must(txn, "SELECT i_price FROM item WHERE i_id = " + std::to_string(item))));
            BENCH_TRY_ASSIGN(qty, cell(must(txn, "SELECT s_quantity FROM stock WHERE s_id = " + std::to_string(item))));
            int64_t next_qty = qty - 5;
            if (next_qty < 10)
            {
                next_qty += 91;
            }
            BENCH_TRY(expect_write(
                must(txn, "UPDATE stock SET s_quantity = " + std::to_string(next_qty) + ", s_order_cnt = s_order_cnt + 1 WHERE s_id = " + std::to_string(item)),
                "stock"));
            int ol = next_line++;
            BENCH_TRY(expect_write(
                must(txn, "INSERT INTO order_line VALUES (" + std::to_string(ol) + "," + std::to_string(oid) + "," + std::to_string(item) + "," + std::to_string(price * 5) + ")"),
                "order_line"));
        }
        BENCH_TRY(txn.commit());
        live_orders.push_back(static_cast<int>(oid));
    }
    record("tpcc", "new_order", kNewOrder, t0);

    t0 = std::chrono::steady_clock::now();
    for (int n = 0; n < kPayment; ++n)
    {
        int d = pick(kDistricts);
        int c = (d - 1) * kCustomersPer + pick(kCustomersPer);
        int amount = 5 + (n % 20);
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY(expect_write(must(txn, "UPDATE warehouse SET w_ytd = w_ytd + " + std::to_string(amount) + " WHERE w_id = 1"), "w"));
        BENCH_TRY(expect_write(must(txn, "UPDATE district SET d_ytd = d_ytd + " + std::to_string(amount) + " WHERE d_id = " + std::to_string(d)), "d"));
        BENCH_TRY(must(txn, "SELECT c_balance, c_last FROM customer WHERE c_id = " + std::to_string(c)));
        BENCH_TRY(expect_write(must(txn, "UPDATE customer SET c_balance = c_balance - " + std::to_string(amount) + " WHERE c_id = " + std::to_string(c)), "c"));
        BENCH_TRY(expect_write(
            must(txn, "INSERT INTO history VALUES (" + std::to_string(next_hist++) + "," + std::to_string(c) + "," + std::to_string(d) + "," + std::to_string(amount) + ")"),
            "h"));
        BENCH_TRY(txn.commit());
    }
    record("tpcc", "payment", kPayment, t0);

    t0 = std::chrono::steady_clock::now();
    int status_ops = static_cast<int>(live_orders.size());
    for (int oid : live_orders)
    {
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY(must(txn, "SELECT o_c_id, o_ol_cnt FROM orders WHERE o_id = " + std::to_string(oid)));
        BENCH_TRY(must(txn, "SELECT ol_i_id, ol_amount FROM order_line WHERE ol_o_id = " + std::to_string(oid)));
        BENCH_TRY(txn.commit());
    }
    record("tpcc", "order_status", status_ops, t0);

    t0 = std::chrono::steady_clock::now();
    int delivered = 0;
    for (int oid : live_orders)
    {
        if (delivered == 15)
        {
            break;
        }
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY_ASSIGN(found, must(txn, "SELECT no_c_id FROM new_order WHERE no_id = " + std::to_string(oid)));
        BENCH_TRY_ASSIGN(cid, cell(found));
        BENCH_TRY(expect_write(must(txn, "DELETE FROM new_order WHERE no_id = " + std::to_string(oid)), "delivery delete"));
        BENCH_TRY(must(txn, "SELECT ol_amount FROM order_line WHERE ol_o_id = " + std::to_string(oid)));
        BENCH_TRY(expect_write(must(txn, "UPDATE customer SET c_balance = c_balance + 10 WHERE c_id = " + std::to_string(cid)), "delivery customer"));
        BENCH_TRY(txn.commit());
        ++delivered;
    }
    record("tpcc", "delivery", delivered, t0);

    t0 = std::chrono::steady_clock::now();
    constexpr int kStockLevel = 20;
    for (int n = 0; n < kStockLevel; ++n)
    {
        BENCH_TRY_ASSIGN(txn, db.begin());
        int low = 0;
        for (int i = 0; i < 20; ++i)
        {
            BENCH_TRY_ASSIGN(qty, cell(must(txn, "SELECT s_quantity FROM stock WHERE s_id = " + std::to_string(pick(kItems)))));
            if (qty < 20)
            {
                ++low;
            }
        }
        (void)low;
        BENCH_TRY(txn.commit());
    }
    record("tpcc", "stock_level", kStockLevel, t0);
    return {};
}

Result<void> run_smallbank()
{
    constexpr int kCustomers = 1000;
    constexpr int kOps = 80;
    std::cout << ">> smallbank\n";
    BENCH_TRY_ASSIGN(dir, prepare_dir("smallbank"));
    BENCH_TRY_ASSIGN(db, SqlRunner::open(dir));
    BENCH_TRY(must(db, "CREATE TABLE accounts (custid INT PRIMARY KEY, name VARCHAR(16))"));
    BENCH_TRY(must(db, "CREATE TABLE savings (custid INT PRIMARY KEY, bal INT)"));
    BENCH_TRY(must(db, "CREATE TABLE checking (custid INT PRIMARY KEY, bal INT)"));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> accounts;
    std::vector<std::string> savings;
    std::vector<std::string> checking;
    accounts.reserve(kCustomers);
    for (int id = 1; id <= kCustomers; ++id)
    {
        accounts.push_back("(" + std::to_string(id) + "," + q("a" + std::to_string(id)) + ")");
        savings.push_back("(" + std::to_string(id) + ",100000)");
        checking.push_back("(" + std::to_string(id) + ",100000)");
    }
    BENCH_TRY(load_values(db, "INSERT INTO accounts VALUES ", accounts, 100));
    BENCH_TRY(load_values(db, "INSERT INTO savings VALUES ", savings, 100));
    BENCH_TRY(load_values(db, "INSERT INTO checking VALUES ", checking, 100));
    record("smallbank", "load", kCustomers, t0);

    auto one = [&](const char * name, auto && body) -> Result<void>
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i)
        {
            BENCH_TRY(body(i));
        }
        record("smallbank", name, kOps, start);
        return {};
    };

    BENCH_TRY(one("amalgamate", [&](int) -> Result<void>
    {
        int a = pick(kCustomers);
        int b = pick(kCustomers);
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY_ASSIGN(sav, cell(must(txn, "SELECT bal FROM savings WHERE custid = " + std::to_string(a))));
        BENCH_TRY_ASSIGN(chk, cell(must(txn, "SELECT bal FROM checking WHERE custid = " + std::to_string(a))));
        BENCH_TRY(expect_write(must(txn, "UPDATE savings SET bal = 0 WHERE custid = " + std::to_string(a)), "sav"));
        BENCH_TRY(expect_write(must(txn, "UPDATE checking SET bal = 0 WHERE custid = " + std::to_string(a)), "chk"));
        BENCH_TRY(expect_write(must(txn, "UPDATE checking SET bal = bal + " + std::to_string(sav + chk) + " WHERE custid = " + std::to_string(b)), "dest"));
        BENCH_TRY(txn.commit());
        return {};
    }));
    BENCH_TRY(one("balance", [&](int) -> Result<void>
    {
        int id = pick(kCustomers);
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY(cell(must(txn, "SELECT bal FROM savings WHERE custid = " + std::to_string(id))));
        BENCH_TRY(cell(must(txn, "SELECT bal FROM checking WHERE custid = " + std::to_string(id))));
        BENCH_TRY(txn.commit());
        return {};
    }));
    BENCH_TRY(one("deposit_checking", [&](int) -> Result<void>
    {
        int id = pick(kCustomers);
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY(expect_write(must(txn, "UPDATE checking SET bal = bal + 10 WHERE custid = " + std::to_string(id)), "dep"));
        BENCH_TRY(txn.commit());
        return {};
    }));
    BENCH_TRY(one("transact_saving", [&](int) -> Result<void>
    {
        int id = pick(kCustomers);
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY(expect_write(must(txn, "UPDATE savings SET bal = bal + 10 WHERE custid = " + std::to_string(id)), "sav"));
        BENCH_TRY(txn.commit());
        return {};
    }));
    BENCH_TRY(one("write_check", [&](int) -> Result<void>
    {
        int id = pick(kCustomers);
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY_ASSIGN(total, cell(must(txn, "SELECT bal FROM savings WHERE custid = " + std::to_string(id))));
        BENCH_TRY_ASSIGN(checking, cell(must(txn, "SELECT bal FROM checking WHERE custid = " + std::to_string(id))));
        total += checking;
        (void)total;
        BENCH_TRY(expect_write(must(txn, "UPDATE checking SET bal = bal - 5 WHERE custid = " + std::to_string(id)), "check"));
        BENCH_TRY(txn.commit());
        return {};
    }));
    return {};
}

Result<void> run_tatp()
{
    constexpr int kSubs = 1000;
    constexpr int kOps = 60;
    std::cout << ">> tatp\n";
    BENCH_TRY_ASSIGN(dir, prepare_dir("tatp"));
    BENCH_TRY_ASSIGN(db, SqlRunner::open(dir));
    BENCH_TRY(must(db, "CREATE TABLE subscriber (s_id INT PRIMARY KEY, sub_nbr VARCHAR(15), bit_1 INT, hex_1 INT, vlr_location INT)"));
    BENCH_TRY(must(db, "CREATE TABLE access_info (s_id INT PRIMARY KEY, data1 INT, data2 INT)"));
    BENCH_TRY(must(db, "CREATE TABLE special_facility (s_id INT PRIMARY KEY, sf_type INT, is_active INT)"));
    BENCH_TRY(must(db, "CREATE TABLE call_forwarding (s_id INT PRIMARY KEY, time_start INT, time_end INT, numberx VARCHAR(15))"));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> subs;
    std::vector<std::string> access;
    std::vector<std::string> facility;
    std::vector<std::string> forward;
    for (int id = 1; id <= kSubs; ++id)
    {
        subs.push_back("(" + std::to_string(id) + "," + q(field(15, id)) + ",1,1,100)");
        access.push_back("(" + std::to_string(id) + ",10,20)");
        facility.push_back("(" + std::to_string(id) + ",1,1)");
        forward.push_back("(" + std::to_string(id) + ",0,24," + q(field(15, id + 3)) + ")");
    }
    BENCH_TRY(load_values(db, "INSERT INTO subscriber VALUES ", subs, 50));
    BENCH_TRY(load_values(db, "INSERT INTO access_info VALUES ", access, 100));
    BENCH_TRY(load_values(db, "INSERT INTO special_facility VALUES ", facility, 100));
    BENCH_TRY(load_values(db, "INSERT INTO call_forwarding VALUES ", forward, 50));
    record("tatp", "load", kSubs, t0);

    auto timed = [&](const char * name, auto && body) -> Result<void>
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i)
        {
            BENCH_TRY(body(i));
        }
        record("tatp", name, kOps, start);
        return {};
    };
    BENCH_TRY(timed("get_subscriber", [&](int) -> Result<void>
    {
        BENCH_TRY_ASSIGN(result, must(db, "SELECT sub_nbr, vlr_location FROM subscriber WHERE s_id = " + std::to_string(pick(kSubs))));
        if (result.rows.size() != 1)
        {
            return popo::base::make_error(Errno::InternalErr, "tatp get_subscriber");
        }
        return {};
    }));
    BENCH_TRY(timed("get_new_destination", [&](int) -> Result<void>
    {
        BENCH_TRY_ASSIGN(result, must(db, "SELECT numberx FROM call_forwarding WHERE s_id = " + std::to_string(pick(kSubs))));
        if (result.rows.size() != 1)
        {
            return popo::base::make_error(Errno::InternalErr, "tatp get_new_destination");
        }
        return {};
    }));
    BENCH_TRY(timed("get_access_data", [&](int) -> Result<void>
    {
        BENCH_TRY(cell(must(db, "SELECT data1 FROM access_info WHERE s_id = " + std::to_string(pick(kSubs)))));
        return {};
    }));
    BENCH_TRY(timed("update_subscriber", [&](int i) -> Result<void>
    {
        std::string sql = "UPDATE subscriber SET bit_1 = " + std::to_string(i % 2) + ", hex_1 = " + std::to_string((i % 16) + 1)
            + " WHERE s_id = " + std::to_string(pick(kSubs));
        BENCH_TRY(expect_write(must(db, sql), sql));
        return {};
    }));
    BENCH_TRY(timed("update_location", [&](int i) -> Result<void>
    {
        std::string sql = "UPDATE subscriber SET vlr_location = " + std::to_string(1000 + i) + " WHERE s_id = " + std::to_string(pick(kSubs));
        BENCH_TRY(expect_write(must(db, sql), sql));
        return {};
    }));
    BENCH_TRY(timed("insert_call_forwarding", [&](int i) -> Result<void>
    {
        int id = kSubs + 1 + i;
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY(expect_write(must(txn, "INSERT INTO subscriber VALUES (" + std::to_string(id) + "," + q(field(15, id)) + ",1,1,1)"), "sub"));
        BENCH_TRY(expect_write(must(txn, "INSERT INTO call_forwarding VALUES (" + std::to_string(id) + ",8,16," + q(field(15, id)) + ")"), "cf"));
        BENCH_TRY(txn.commit());
        return {};
    }));
    BENCH_TRY(timed("delete_call_forwarding", [&](int i) -> Result<void>
    {
        int id = kSubs + 1 + i;
        std::string sql = "DELETE FROM call_forwarding WHERE s_id = " + std::to_string(id);
        BENCH_TRY(expect_write(must(db, sql), sql));
        return {};
    }));
    return {};
}

Result<void> run_tpce()
{
    constexpr int kAccounts = 500;
    constexpr int kSecurities = 100;
    constexpr int kTxns = 40;
    std::cout << ">> tpce\n";
    BENCH_TRY_ASSIGN(dir, prepare_dir("tpce"));
    BENCH_TRY_ASSIGN(db, SqlRunner::open(dir));
    BENCH_TRY(must(db, "CREATE TABLE customer_account (ca_id INT PRIMARY KEY, balance INT)"));
    BENCH_TRY(must(db, "CREATE TABLE security (s_id INT PRIMARY KEY, price INT)"));
    BENCH_TRY(must(db, "CREATE TABLE holding (h_id INT PRIMARY KEY, qty INT)"));
    BENCH_TRY(must(db, "CREATE TABLE trade (t_id INT PRIMARY KEY, ca_id INT, s_id INT, qty INT, price INT)"));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> accounts;
    std::vector<std::string> holdings;
    for (int id = 1; id <= kAccounts; ++id)
    {
        accounts.push_back("(" + std::to_string(id) + ",1000000)");
        holdings.push_back("(" + std::to_string(id) + ",100)");
    }
    BENCH_TRY(load_values(db, "INSERT INTO customer_account VALUES ", accounts, 100));
    BENCH_TRY(load_values(db, "INSERT INTO holding VALUES ", holdings, 100));
    std::vector<std::string> secs;
    for (int id = 1; id <= kSecurities; ++id)
    {
        secs.push_back("(" + std::to_string(id) + "," + std::to_string(20 + id) + ")");
    }
    BENCH_TRY(load_values(db, "INSERT INTO security VALUES ", secs, 50));
    record("tpce", "load", kAccounts, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kTxns; ++i)
    {
        int ca = pick(kAccounts);
        int sec = pick(kSecurities);
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY(cell(must(txn, "SELECT balance FROM customer_account WHERE ca_id = " + std::to_string(ca))));
        BENCH_TRY_ASSIGN(price, cell(must(txn, "SELECT price FROM security WHERE s_id = " + std::to_string(sec))));
        BENCH_TRY(expect_write(must(txn, "UPDATE holding SET qty = qty + 1 WHERE h_id = " + std::to_string(ca)), "holding"));
        BENCH_TRY(expect_write(must(txn, "UPDATE customer_account SET balance = balance - " + std::to_string(price) + " WHERE ca_id = " + std::to_string(ca)), "cash"));
        BENCH_TRY(expect_write(
            must(txn, "INSERT INTO trade VALUES (" + std::to_string(i + 1) + "," + std::to_string(ca) + "," + std::to_string(sec) + ",1," + std::to_string(price) + ")"),
            "trade"));
        BENCH_TRY(txn.commit());
    }
    record("tpce", "trade_order", kTxns, t0);
    return {};
}

Result<void> run_wikipedia()
{
    constexpr int kPages = 400;
    constexpr int kUsers = 50;
    constexpr int kOps = 40;
    std::cout << ">> wikipedia\n";
    BENCH_TRY_ASSIGN(dir, prepare_dir("wikipedia"));
    BENCH_TRY_ASSIGN(db, SqlRunner::open(dir));
    BENCH_TRY(must(db, "CREATE TABLE useracct (user_id INT PRIMARY KEY, user_name VARCHAR(16))"));
    BENCH_TRY(must(db, "CREATE TABLE page (page_id INT PRIMARY KEY, latest INT, page_title VARCHAR(32))"));
    BENCH_TRY(must(db, "CREATE TABLE revision (rev_id INT PRIMARY KEY, page_id INT, user_id INT, rev_text VARCHAR(64))"));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> users;
    for (int id = 1; id <= kUsers; ++id)
    {
        users.push_back("(" + std::to_string(id) + "," + q("u" + std::to_string(id)) + ")");
    }
    BENCH_TRY(load_values(db, "INSERT INTO useracct VALUES ", users, 50));
    std::vector<std::string> pages;
    std::vector<std::string> revs;
    for (int id = 1; id <= kPages; ++id)
    {
        pages.push_back("(" + std::to_string(id) + ",0," + q("p" + std::to_string(id)) + ")");
        revs.push_back("(" + std::to_string(id) + "," + std::to_string(id) + ",1," + q(field(64, id)) + ")");
    }
    BENCH_TRY(load_values(db, "INSERT INTO page VALUES ", pages, 50));
    BENCH_TRY(load_values(db, "INSERT INTO revision VALUES ", revs, 40));
    record("wikipedia", "load", kPages, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int page = pick(kPages);
        int user = pick(kUsers);
        int rev = kPages + 1 + i;
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY(must(txn, "SELECT user_name FROM useracct WHERE user_id = " + std::to_string(user)));
        BENCH_TRY(must(txn, "SELECT latest FROM page WHERE page_id = " + std::to_string(page)));
        BENCH_TRY(expect_write(
            must(txn, "INSERT INTO revision VALUES (" + std::to_string(rev) + "," + std::to_string(page) + "," + std::to_string(user) + "," + q(field(64, rev)) + ")"),
            "rev"));
        BENCH_TRY(expect_write(must(txn, "UPDATE page SET latest = " + std::to_string(rev) + " WHERE page_id = " + std::to_string(page)), "page"));
        BENCH_TRY(txn.commit());
    }
    record("wikipedia", "update_page", kOps, t0);
    return {};
}

Result<void> run_auctionmark()
{
    constexpr int kItems = 400;
    constexpr int kOps = 40;
    std::cout << ">> auctionmark\n";
    BENCH_TRY_ASSIGN(dir, prepare_dir("auctionmark"));
    BENCH_TRY_ASSIGN(db, SqlRunner::open(dir));
    BENCH_TRY(must(db, "CREATE TABLE item (i_id INT PRIMARY KEY, price INT, num_bids INT, i_name VARCHAR(24))"));
    BENCH_TRY(must(db, "CREATE TABLE bid (b_id INT PRIMARY KEY, i_id INT, amount INT)"));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> items;
    for (int id = 1; id <= kItems; ++id)
    {
        items.push_back("(" + std::to_string(id) + ",100,0," + q("item" + std::to_string(id)) + ")");
    }
    BENCH_TRY(load_values(db, "INSERT INTO item VALUES ", items, 50));
    record("auctionmark", "load", kItems, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int item = pick(kItems);
        BENCH_TRY_ASSIGN(txn, db.begin());
        BENCH_TRY_ASSIGN(price, cell(must(txn, "SELECT price FROM item WHERE i_id = " + std::to_string(item))));
        int64_t bid = price + 1 + (i % 5);
        BENCH_TRY(expect_write(
            must(txn, "UPDATE item SET price = " + std::to_string(bid) + ", num_bids = num_bids + 1 WHERE i_id = " + std::to_string(item)),
            "item"));
        BENCH_TRY(expect_write(must(txn, "INSERT INTO bid VALUES (" + std::to_string(i + 1) + "," + std::to_string(item) + "," + std::to_string(bid) + ")"), "bid"));
        BENCH_TRY(txn.commit());
    }
    record("auctionmark", "new_bid", kOps, t0);
    return {};
}

Result<void> run(int argc, char ** argv)
{
        std::error_code ec;
        std::filesystem::create_directories(kDataRoot, ec);
        if (ec)
            return popo::base::make_error(Errno::IoErr, "create data root: " + ec.message());
        std::cout << "engine=sqlite " << sqlite3_libversion() << "\n";
        std::cout << "pragmas=journal_mode=DELETE synchronous=FULL temp_store=MEMORY\n";
        std::cout << "data_dir=" << kDataRoot << "\n";
        BENCH_TRY_ASSIGN(mount, mount_of(kDataRoot));
        std::cout << "filesystem=" << mount << "\n";
        std::string only = argc > 1 ? argv[1] : "";
        auto want = [&](const char * name) { return only.empty() || only == name; };
        if (want("sysbench"))
            BENCH_TRY(run_sysbench());
        if (want("pgbench"))
            BENCH_TRY(run_pgbench());
        if (want("ycsb"))
            BENCH_TRY(run_ycsb());
        if (want("tpcc"))
            BENCH_TRY(run_tpcc());
        if (want("smallbank"))
            BENCH_TRY(run_smallbank());
        if (want("tatp"))
            BENCH_TRY(run_tatp());
        if (want("tpce"))
            BENCH_TRY(run_tpce());
        if (want("wikipedia"))
            BENCH_TRY(run_wikipedia());
        if (want("auctionmark"))
            BENCH_TRY(run_auctionmark());

        std::cout << "\nsuite        workload                  ops        ms       tps\n";
        for (const auto & s : g_samples)
        {
            double tps = s.ms > 0 ? (s.ops * 1000.0 / s.ms) : 0;
            std::cout << s.suite << "  " << s.workload << "  " << s.ops << "  " << s.ms << "  " << tps << "\n";
        }
        BENCH_TRY_ASSIGN(bytes, dir_bytes(kDataRoot));
        std::cout << "bytes=" << bytes << "\n";
        return {};
}

int main(int argc, char ** argv)
{
    auto result = run(argc, argv);
    if (!result)
    {
        std::cerr << "sqlite_oltp failed: " << result.error().error_message << "\n";
        return 1;
    }
    return 0;
}
