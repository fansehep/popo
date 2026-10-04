#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    ExecuteSuccess exec(const std::string & sql);
    void commit();

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
    explicit SqlRunner(const std::string & dir)
    {
        std::filesystem::path path = std::filesystem::path(dir) / "bench.db";
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
        {
            throw std::runtime_error(std::string("sqlite open: ") + sqlite3_errmsg(db_));
        }
        exec("PRAGMA journal_mode=DELETE");
        exec("PRAGMA synchronous=FULL");
        exec("PRAGMA temp_store=MEMORY");
        exec("PRAGMA cache_size=-65536");
    }

    ~SqlRunner()
    {
        if (db_ != nullptr)
        {
            sqlite3_close(db_);
        }
    }

    ExecuteSuccess exec(const std::string & sql) { return run(sql); }

    SqlTxn begin()
    {
        exec("BEGIN IMMEDIATE");
        return SqlTxn(this);
    }

private:
    friend class SqlTxn;

    ExecuteSuccess run(const std::string & sql)
    {
        ExecuteSuccess out;
        const char * tail = sql.c_str();
        while (tail != nullptr && *tail != '\0')
        {
            sqlite3_stmt * stmt = nullptr;
            int rc = sqlite3_prepare_v2(db_, tail, -1, &stmt, &tail);
            if (rc != SQLITE_OK)
            {
                throw std::runtime_error(sql + "\n" + sqlite3_errmsg(db_));
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
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE)
            {
                throw std::runtime_error(sql + "\n" + sqlite3_errmsg(db_));
            }
            out.rows_affected = sqlite3_changes(db_);
        }
        return out;
    }

    sqlite3 * db_ = nullptr;
};

SqlTxn::~SqlTxn()
{
    if (open_ && runner_ != nullptr)
    {
        try
        {
            runner_->run("ROLLBACK");
        }
        catch (...)
        {
        }
        open_ = false;
    }
}

ExecuteSuccess SqlTxn::exec(const std::string & sql)
{
    if (!open_ || runner_ == nullptr)
    {
        throw std::runtime_error("transaction is closed");
    }
    try
    {
        return runner_->run(sql);
    }
    catch (...)
    {
        runner_->run("ROLLBACK");
        open_ = false;
        throw;
    }
}

void SqlTxn::commit()
{
    if (!open_ || runner_ == nullptr)
    {
        throw std::runtime_error("transaction is closed");
    }
    runner_->run("COMMIT");
    open_ = false;
}

ExecuteSuccess must(SqlRunner & db, const std::string & sql)
{
    return db.exec(sql);
}

ExecuteSuccess must(SqlTxn & txn, const std::string & sql)
{
    return txn.exec(sql);
}

void expect_write(const ExecuteSuccess & result, const std::string & sql)
{
    if (result.rows_affected < 1)
    {
        throw std::runtime_error("no row written: " + sql);
    }
}

int64_t cell(const ExecuteSuccess & result, size_t col = 0)
{
    if (result.rows.empty() || col >= result.rows[0]->values_.size())
    {
        throw std::runtime_error("empty result");
    }
    return result.rows[0]->values_[col];
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

void load_values(SqlRunner & db, const std::string & prefix, const std::vector<std::string> & tuples, int batch)
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
        auto result = must(db, sql);
        if (result.rows_affected != n)
        {
            throw std::runtime_error("insert batch wrote " + std::to_string(result.rows_affected) + " rows, expected " + std::to_string(n));
        }
    }
}

std::string prepare_dir(const std::string & name)
{
    std::filesystem::path dir = std::filesystem::path(kDataRoot) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

std::string mount_of(const std::string & path)
{
    std::ifstream in("/proc/mounts");
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
        throw std::runtime_error("cannot resolve filesystem for " + path);
    }
    if (best_fs == "tmpfs")
    {
        throw std::runtime_error("refusing tmpfs at " + best_point);
    }
    return best_dev + " " + best_fs + " at " + best_point;
}

uint64_t dir_bytes(const std::filesystem::path & root)
{
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (it->is_regular_file(ec))
        {
            n += it->file_size(ec);
        }
    }
    return n;
}

void run_sysbench()
{
    constexpr int kRows = 2000;
    constexpr int kOps = 200;
    std::cout << ">> sysbench\n";
    SqlRunner db(prepare_dir("sysbench"));
    must(db, "CREATE TABLE sbtest1 (id INT PRIMARY KEY, k INT, c VARCHAR(120), pad VARCHAR(60))");

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> tuples;
    tuples.reserve(kRows);
    for (int id = 1; id <= kRows; ++id)
    {
        tuples.push_back("(" + std::to_string(id) + "," + std::to_string(id % 1000) + "," + q(field(120, id)) + "," + q(field(60, id + 7)) + ")");
    }
    load_values(db, "INSERT INTO sbtest1 VALUES ", tuples, 50);
    record("sysbench", "load", kRows, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        auto result = must(db, "SELECT c FROM sbtest1 WHERE id = " + std::to_string(id));
        if (result.rows.size() != 1)
        {
            throw std::runtime_error("sysbench point select missed id");
        }
    }
    record("sysbench", "oltp_point_select", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        std::string sql = "UPDATE sbtest1 SET k = k + 1 WHERE id = " + std::to_string(pick(kRows));
        expect_write(must(db, sql), sql);
    }
    record("sysbench", "oltp_update_index", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        std::string sql = "UPDATE sbtest1 SET c = " + q(field(120, 100000 + i)) + " WHERE id = " + std::to_string(id);
        expect_write(must(db, sql), sql);
    }
    record("sysbench", "oltp_update_non_index", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = kRows + 1 + i;
        std::string sql = "INSERT INTO sbtest1 VALUES (" + std::to_string(id) + "," + std::to_string(i) + "," + q(field(120, id)) + ","
            + q(field(60, id)) + ")";
        expect_write(must(db, sql), sql);
    }
    record("sysbench", "oltp_insert", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = kRows + 1 + i;
        std::string sql = "DELETE FROM sbtest1 WHERE id = " + std::to_string(id);
        expect_write(must(db, sql), sql);
    }
    record("sysbench", "oltp_delete", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    constexpr int kRange = 40;
    for (int i = 0; i < kRange; ++i)
    {
        int lo = pick(kRows - 20);
        must(db, "SELECT c FROM sbtest1 WHERE id >= " + std::to_string(lo) + " AND id <= " + std::to_string(lo + 10));
    }
    record("sysbench", "oltp_range_select", kRange, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        auto txn = db.begin();
        for (int s = 0; s < 10; ++s)
        {
            must(txn, "SELECT c FROM sbtest1 WHERE id = " + std::to_string(pick(kRows)));
        }
        must(txn, "SELECT c FROM sbtest1 WHERE id >= " + std::to_string(id) + " AND id <= " + std::to_string(id + 5));
        expect_write(must(txn, "UPDATE sbtest1 SET k = k + 1 WHERE id = " + std::to_string(id)), "rw update k");
        expect_write(must(txn, "UPDATE sbtest1 SET c = " + q(field(120, 200000 + i)) + " WHERE id = " + std::to_string(id)), "rw update c");
        int fresh = kRows + 10000 + i;
        expect_write(
            must(txn, "INSERT INTO sbtest1 VALUES (" + std::to_string(fresh) + ",1," + q(field(120, fresh)) + "," + q(field(60, fresh)) + ")"),
            "rw insert");
        expect_write(must(txn, "DELETE FROM sbtest1 WHERE id = " + std::to_string(fresh)), "rw delete");
        txn.commit();
    }
    record("sysbench", "oltp_read_write", kOps, t0);
}

void run_pgbench()
{
    constexpr int kAccounts = 2000;
    constexpr int kTellers = 10;
    constexpr int kTxns = 200;
    std::cout << ">> pgbench\n";
    SqlRunner db(prepare_dir("pgbench"));
    must(db, "CREATE TABLE pgbench_branches (bid INT PRIMARY KEY, bbalance INT)");
    must(db, "CREATE TABLE pgbench_tellers (tid INT PRIMARY KEY, bid INT, tbalance INT)");
    must(db, "CREATE TABLE pgbench_accounts (aid INT PRIMARY KEY, bid INT, abalance INT)");
    must(db, "CREATE TABLE pgbench_history (hid INT PRIMARY KEY, tid INT, bid INT, aid INT, delta INT)");

    auto t0 = std::chrono::steady_clock::now();
    must(db, "INSERT INTO pgbench_branches VALUES (1,0)");
    std::vector<std::string> tellers;
    for (int tid = 1; tid <= kTellers; ++tid)
    {
        tellers.push_back("(" + std::to_string(tid) + ",1,0)");
    }
    load_values(db, "INSERT INTO pgbench_tellers VALUES ", tellers, 10);
    std::vector<std::string> accounts;
    accounts.reserve(kAccounts);
    for (int aid = 1; aid <= kAccounts; ++aid)
    {
        accounts.push_back("(" + std::to_string(aid) + ",1,0)");
    }
    load_values(db, "INSERT INTO pgbench_accounts VALUES ", accounts, 100);
    record("pgbench", "load", kAccounts, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kTxns; ++i)
    {
        int aid = pick(kAccounts);
        int tid = pick(kTellers);
        int delta = (i % 2 == 0) ? 1 : -1;
        auto txn = db.begin();
        expect_write(must(txn, "UPDATE pgbench_accounts SET abalance = abalance + " + std::to_string(delta) + " WHERE aid = " + std::to_string(aid)), "account");
        must(txn, "SELECT abalance FROM pgbench_accounts WHERE aid = " + std::to_string(aid));
        expect_write(must(txn, "UPDATE pgbench_tellers SET tbalance = tbalance + " + std::to_string(delta) + " WHERE tid = " + std::to_string(tid)), "teller");
        expect_write(must(txn, "UPDATE pgbench_branches SET bbalance = bbalance + " + std::to_string(delta) + " WHERE bid = 1"), "branch");
        expect_write(
            must(txn, "INSERT INTO pgbench_history VALUES (" + std::to_string(i + 1) + "," + std::to_string(tid) + ",1," + std::to_string(aid) + "," + std::to_string(delta) + ")"),
            "history");
        txn.commit();
    }
    record("pgbench", "tpcb", kTxns, t0);
}

void run_ycsb()
{
    constexpr int kRows = 1000;
    constexpr int kOps = 200;
    constexpr int kFields = 10;
    constexpr int kWidth = 100;
    std::cout << ">> ycsb\n";
    SqlRunner db(prepare_dir("ycsb"));
    std::string ddl = "CREATE TABLE usertable (id INT PRIMARY KEY";
    for (int f = 0; f < kFields; ++f)
    {
        ddl += ", f" + std::to_string(f) + " VARCHAR(" + std::to_string(kWidth) + ")";
    }
    ddl += ")";
    must(db, ddl);

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
    load_values(db, "INSERT INTO usertable VALUES ", tuples, 20);
    record("ycsb", "load", kRows, t0);

    auto mix = [&](const char * name, int reads_per_100)
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i)
        {
            int id = pick(kRows);
            bool read = (i % 100) < reads_per_100;
            if (read)
            {
                auto result = must(db, "SELECT f0 FROM usertable WHERE id = " + std::to_string(id));
                if (result.rows.size() != 1)
                {
                    throw std::runtime_error("ycsb read missed");
                }
            }
            else
            {
                std::string sql = "UPDATE usertable SET f0 = " + q(field(kWidth, 500000 + i)) + " WHERE id = " + std::to_string(id);
                expect_write(must(db, sql), sql);
            }
        }
        record("ycsb", name, kOps, start);
    };
    mix("workload_a", 50);
    mix("workload_b", 95);
    mix("workload_c", 100);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        auto txn = db.begin();
        auto result = must(txn, "SELECT f0 FROM usertable WHERE id = " + std::to_string(id));
        if (result.rows.size() != 1)
        {
            throw std::runtime_error("ycsb rmw missed");
        }
        expect_write(must(txn, "UPDATE usertable SET f0 = " + q(field(kWidth, 800000 + i)) + " WHERE id = " + std::to_string(id)), "ycsb rmw");
        txn.commit();
    }
    record("ycsb", "workload_f", kOps, start);
}

void run_tpcc()
{
    constexpr int kDistricts = 4;
    constexpr int kCustomersPer = 50;
    constexpr int kItems = 200;
    constexpr int kNewOrder = 30;
    constexpr int kPayment = 30;
    constexpr int kCustomers = kDistricts * kCustomersPer;
    std::cout << ">> tpcc\n";
    SqlRunner db(prepare_dir("tpcc"));
    must(db, "CREATE TABLE warehouse (w_id INT PRIMARY KEY, w_ytd INT, w_name VARCHAR(16))");
    must(db, "CREATE TABLE district (d_id INT PRIMARY KEY, d_ytd INT, d_next_o_id INT, d_name VARCHAR(16))");
    must(db, "CREATE TABLE customer (c_id INT PRIMARY KEY, c_d_id INT, c_balance INT, c_last VARCHAR(16))");
    must(db, "CREATE TABLE item (i_id INT PRIMARY KEY, i_price INT, i_name VARCHAR(24))");
    must(db, "CREATE TABLE stock (s_id INT PRIMARY KEY, s_quantity INT, s_order_cnt INT)");
    must(db, "CREATE TABLE orders (o_id INT PRIMARY KEY, o_d_id INT, o_c_id INT, o_ol_cnt INT)");
    must(db, "CREATE TABLE new_order (no_id INT PRIMARY KEY, no_d_id INT, no_c_id INT)");
    must(db, "CREATE TABLE order_line (ol_id INT PRIMARY KEY, ol_o_id INT, ol_i_id INT, ol_amount INT)");
    must(db, "CREATE TABLE history (h_id INT PRIMARY KEY, h_c_id INT, h_d_id INT, h_amount INT)");

    auto t0 = std::chrono::steady_clock::now();
    must(db, "INSERT INTO warehouse VALUES (1,0,'w1')");
    std::vector<std::string> rows;
    for (int d = 1; d <= kDistricts; ++d)
    {
        rows.push_back("(" + std::to_string(d) + ",0,1," + q("d" + std::to_string(d)) + ")");
    }
    load_values(db, "INSERT INTO district VALUES ", rows, 10);
    rows.clear();
    for (int c = 1; c <= kCustomers; ++c)
    {
        int d = ((c - 1) / kCustomersPer) + 1;
        rows.push_back("(" + std::to_string(c) + "," + std::to_string(d) + ",1000," + q("c" + std::to_string(c)) + ")");
    }
    load_values(db, "INSERT INTO customer VALUES ", rows, 50);
    rows.clear();
    for (int i = 1; i <= kItems; ++i)
    {
        rows.push_back("(" + std::to_string(i) + "," + std::to_string(10 + (i % 50)) + "," + q("item" + std::to_string(i)) + ")");
    }
    load_values(db, "INSERT INTO item VALUES ", rows, 50);
    rows.clear();
    for (int i = 1; i <= kItems; ++i)
    {
        rows.push_back("(" + std::to_string(i) + ",50,0)");
    }
    load_values(db, "INSERT INTO stock VALUES ", rows, 50);
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
        auto txn = db.begin();
        must(txn, "SELECT w_ytd FROM warehouse WHERE w_id = 1");
        int64_t seq = cell(must(txn, "SELECT d_next_o_id FROM district WHERE d_id = " + std::to_string(d)));
        int64_t oid = seq * 100 + d;
        expect_write(must(txn, "UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = " + std::to_string(d)), "district");
        expect_write(
            must(txn, "INSERT INTO orders VALUES (" + std::to_string(oid) + "," + std::to_string(d) + "," + std::to_string(c) + "," + std::to_string(kLines) + ")"),
            "orders");
        expect_write(must(txn, "INSERT INTO new_order VALUES (" + std::to_string(oid) + "," + std::to_string(d) + "," + std::to_string(c) + ")"), "new_order");
        for (int line = 0; line < kLines; ++line)
        {
            int item = pick(kItems);
            int64_t price = cell(must(txn, "SELECT i_price FROM item WHERE i_id = " + std::to_string(item)));
            int64_t qty = cell(must(txn, "SELECT s_quantity FROM stock WHERE s_id = " + std::to_string(item)));
            int64_t next_qty = qty - 5;
            if (next_qty < 10)
            {
                next_qty += 91;
            }
            expect_write(
                must(txn, "UPDATE stock SET s_quantity = " + std::to_string(next_qty) + ", s_order_cnt = s_order_cnt + 1 WHERE s_id = " + std::to_string(item)),
                "stock");
            int ol = next_line++;
            expect_write(
                must(txn, "INSERT INTO order_line VALUES (" + std::to_string(ol) + "," + std::to_string(oid) + "," + std::to_string(item) + "," + std::to_string(price * 5) + ")"),
                "order_line");
        }
        txn.commit();
        live_orders.push_back(static_cast<int>(oid));
    }
    record("tpcc", "new_order", kNewOrder, t0);

    t0 = std::chrono::steady_clock::now();
    for (int n = 0; n < kPayment; ++n)
    {
        int d = pick(kDistricts);
        int c = (d - 1) * kCustomersPer + pick(kCustomersPer);
        int amount = 5 + (n % 20);
        auto txn = db.begin();
        expect_write(must(txn, "UPDATE warehouse SET w_ytd = w_ytd + " + std::to_string(amount) + " WHERE w_id = 1"), "w");
        expect_write(must(txn, "UPDATE district SET d_ytd = d_ytd + " + std::to_string(amount) + " WHERE d_id = " + std::to_string(d)), "d");
        must(txn, "SELECT c_balance, c_last FROM customer WHERE c_id = " + std::to_string(c));
        expect_write(must(txn, "UPDATE customer SET c_balance = c_balance - " + std::to_string(amount) + " WHERE c_id = " + std::to_string(c)), "c");
        expect_write(
            must(txn, "INSERT INTO history VALUES (" + std::to_string(next_hist++) + "," + std::to_string(c) + "," + std::to_string(d) + "," + std::to_string(amount) + ")"),
            "h");
        txn.commit();
    }
    record("tpcc", "payment", kPayment, t0);

    t0 = std::chrono::steady_clock::now();
    int status_ops = static_cast<int>(live_orders.size());
    for (int oid : live_orders)
    {
        auto txn = db.begin();
        must(txn, "SELECT o_c_id, o_ol_cnt FROM orders WHERE o_id = " + std::to_string(oid));
        must(txn, "SELECT ol_i_id, ol_amount FROM order_line WHERE ol_o_id = " + std::to_string(oid));
        txn.commit();
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
        auto txn = db.begin();
        auto found = must(txn, "SELECT no_c_id FROM new_order WHERE no_id = " + std::to_string(oid));
        int64_t cid = cell(found);
        expect_write(must(txn, "DELETE FROM new_order WHERE no_id = " + std::to_string(oid)), "delivery delete");
        must(txn, "SELECT ol_amount FROM order_line WHERE ol_o_id = " + std::to_string(oid));
        expect_write(must(txn, "UPDATE customer SET c_balance = c_balance + 10 WHERE c_id = " + std::to_string(cid)), "delivery customer");
        txn.commit();
        ++delivered;
    }
    record("tpcc", "delivery", delivered, t0);

    t0 = std::chrono::steady_clock::now();
    constexpr int kStockLevel = 20;
    for (int n = 0; n < kStockLevel; ++n)
    {
        auto txn = db.begin();
        int low = 0;
        for (int i = 0; i < 20; ++i)
        {
            int64_t qty = cell(must(txn, "SELECT s_quantity FROM stock WHERE s_id = " + std::to_string(pick(kItems))));
            if (qty < 20)
            {
                ++low;
            }
        }
        (void)low;
        txn.commit();
    }
    record("tpcc", "stock_level", kStockLevel, t0);
}

void run_smallbank()
{
    constexpr int kCustomers = 1000;
    constexpr int kOps = 80;
    std::cout << ">> smallbank\n";
    SqlRunner db(prepare_dir("smallbank"));
    must(db, "CREATE TABLE accounts (custid INT PRIMARY KEY, name VARCHAR(16))");
    must(db, "CREATE TABLE savings (custid INT PRIMARY KEY, bal INT)");
    must(db, "CREATE TABLE checking (custid INT PRIMARY KEY, bal INT)");

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
    load_values(db, "INSERT INTO accounts VALUES ", accounts, 100);
    load_values(db, "INSERT INTO savings VALUES ", savings, 100);
    load_values(db, "INSERT INTO checking VALUES ", checking, 100);
    record("smallbank", "load", kCustomers, t0);

    auto one = [&](const char * name, auto && body)
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i)
        {
            body(i);
        }
        record("smallbank", name, kOps, start);
    };

    one("amalgamate", [&](int)
    {
        int a = pick(kCustomers);
        int b = pick(kCustomers);
        auto txn = db.begin();
        int64_t sav = cell(must(txn, "SELECT bal FROM savings WHERE custid = " + std::to_string(a)));
        int64_t chk = cell(must(txn, "SELECT bal FROM checking WHERE custid = " + std::to_string(a)));
        expect_write(must(txn, "UPDATE savings SET bal = 0 WHERE custid = " + std::to_string(a)), "sav");
        expect_write(must(txn, "UPDATE checking SET bal = 0 WHERE custid = " + std::to_string(a)), "chk");
        expect_write(must(txn, "UPDATE checking SET bal = bal + " + std::to_string(sav + chk) + " WHERE custid = " + std::to_string(b)), "dest");
        txn.commit();
    });
    one("balance", [&](int)
    {
        int id = pick(kCustomers);
        auto txn = db.begin();
        cell(must(txn, "SELECT bal FROM savings WHERE custid = " + std::to_string(id)));
        cell(must(txn, "SELECT bal FROM checking WHERE custid = " + std::to_string(id)));
        txn.commit();
    });
    one("deposit_checking", [&](int)
    {
        int id = pick(kCustomers);
        auto txn = db.begin();
        expect_write(must(txn, "UPDATE checking SET bal = bal + 10 WHERE custid = " + std::to_string(id)), "dep");
        txn.commit();
    });
    one("transact_saving", [&](int)
    {
        int id = pick(kCustomers);
        auto txn = db.begin();
        expect_write(must(txn, "UPDATE savings SET bal = bal + 10 WHERE custid = " + std::to_string(id)), "sav");
        txn.commit();
    });
    one("write_check", [&](int)
    {
        int id = pick(kCustomers);
        auto txn = db.begin();
        int64_t total = cell(must(txn, "SELECT bal FROM savings WHERE custid = " + std::to_string(id)));
        total += cell(must(txn, "SELECT bal FROM checking WHERE custid = " + std::to_string(id)));
        (void)total;
        expect_write(must(txn, "UPDATE checking SET bal = bal - 5 WHERE custid = " + std::to_string(id)), "check");
        txn.commit();
    });
}

void run_tatp()
{
    constexpr int kSubs = 1000;
    constexpr int kOps = 60;
    std::cout << ">> tatp\n";
    SqlRunner db(prepare_dir("tatp"));
    must(db, "CREATE TABLE subscriber (s_id INT PRIMARY KEY, sub_nbr VARCHAR(15), bit_1 INT, hex_1 INT, vlr_location INT)");
    must(db, "CREATE TABLE access_info (s_id INT PRIMARY KEY, data1 INT, data2 INT)");
    must(db, "CREATE TABLE special_facility (s_id INT PRIMARY KEY, sf_type INT, is_active INT)");
    must(db, "CREATE TABLE call_forwarding (s_id INT PRIMARY KEY, time_start INT, time_end INT, numberx VARCHAR(15))");

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
    load_values(db, "INSERT INTO subscriber VALUES ", subs, 50);
    load_values(db, "INSERT INTO access_info VALUES ", access, 100);
    load_values(db, "INSERT INTO special_facility VALUES ", facility, 100);
    load_values(db, "INSERT INTO call_forwarding VALUES ", forward, 50);
    record("tatp", "load", kSubs, t0);

    auto timed = [&](const char * name, auto && body)
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i)
        {
            body(i);
        }
        record("tatp", name, kOps, start);
    };
    timed("get_subscriber", [&](int)
    {
        auto result = must(db, "SELECT sub_nbr, vlr_location FROM subscriber WHERE s_id = " + std::to_string(pick(kSubs)));
        if (result.rows.size() != 1)
        {
            throw std::runtime_error("tatp get_subscriber");
        }
    });
    timed("get_new_destination", [&](int)
    {
        auto result = must(db, "SELECT numberx FROM call_forwarding WHERE s_id = " + std::to_string(pick(kSubs)));
        if (result.rows.size() != 1)
        {
            throw std::runtime_error("tatp get_new_destination");
        }
    });
    timed("get_access_data", [&](int)
    {
        cell(must(db, "SELECT data1 FROM access_info WHERE s_id = " + std::to_string(pick(kSubs))));
    });
    timed("update_subscriber", [&](int i)
    {
        std::string sql = "UPDATE subscriber SET bit_1 = " + std::to_string(i % 2) + ", hex_1 = " + std::to_string((i % 16) + 1)
            + " WHERE s_id = " + std::to_string(pick(kSubs));
        expect_write(must(db, sql), sql);
    });
    timed("update_location", [&](int i)
    {
        std::string sql = "UPDATE subscriber SET vlr_location = " + std::to_string(1000 + i) + " WHERE s_id = " + std::to_string(pick(kSubs));
        expect_write(must(db, sql), sql);
    });
    timed("insert_call_forwarding", [&](int i)
    {
        int id = kSubs + 1 + i;
        auto txn = db.begin();
        expect_write(must(txn, "INSERT INTO subscriber VALUES (" + std::to_string(id) + "," + q(field(15, id)) + ",1,1,1)"), "sub");
        expect_write(must(txn, "INSERT INTO call_forwarding VALUES (" + std::to_string(id) + ",8,16," + q(field(15, id)) + ")"), "cf");
        txn.commit();
    });
    timed("delete_call_forwarding", [&](int i)
    {
        int id = kSubs + 1 + i;
        std::string sql = "DELETE FROM call_forwarding WHERE s_id = " + std::to_string(id);
        expect_write(must(db, sql), sql);
    });
}

void run_tpce()
{
    constexpr int kAccounts = 500;
    constexpr int kSecurities = 100;
    constexpr int kTxns = 40;
    std::cout << ">> tpce\n";
    SqlRunner db(prepare_dir("tpce"));
    must(db, "CREATE TABLE customer_account (ca_id INT PRIMARY KEY, balance INT)");
    must(db, "CREATE TABLE security (s_id INT PRIMARY KEY, price INT)");
    must(db, "CREATE TABLE holding (h_id INT PRIMARY KEY, qty INT)");
    must(db, "CREATE TABLE trade (t_id INT PRIMARY KEY, ca_id INT, s_id INT, qty INT, price INT)");

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> accounts;
    std::vector<std::string> holdings;
    for (int id = 1; id <= kAccounts; ++id)
    {
        accounts.push_back("(" + std::to_string(id) + ",1000000)");
        holdings.push_back("(" + std::to_string(id) + ",100)");
    }
    load_values(db, "INSERT INTO customer_account VALUES ", accounts, 100);
    load_values(db, "INSERT INTO holding VALUES ", holdings, 100);
    std::vector<std::string> secs;
    for (int id = 1; id <= kSecurities; ++id)
    {
        secs.push_back("(" + std::to_string(id) + "," + std::to_string(20 + id) + ")");
    }
    load_values(db, "INSERT INTO security VALUES ", secs, 50);
    record("tpce", "load", kAccounts, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kTxns; ++i)
    {
        int ca = pick(kAccounts);
        int sec = pick(kSecurities);
        auto txn = db.begin();
        cell(must(txn, "SELECT balance FROM customer_account WHERE ca_id = " + std::to_string(ca)));
        int64_t price = cell(must(txn, "SELECT price FROM security WHERE s_id = " + std::to_string(sec)));
        expect_write(must(txn, "UPDATE holding SET qty = qty + 1 WHERE h_id = " + std::to_string(ca)), "holding");
        expect_write(must(txn, "UPDATE customer_account SET balance = balance - " + std::to_string(price) + " WHERE ca_id = " + std::to_string(ca)), "cash");
        expect_write(
            must(txn, "INSERT INTO trade VALUES (" + std::to_string(i + 1) + "," + std::to_string(ca) + "," + std::to_string(sec) + ",1," + std::to_string(price) + ")"),
            "trade");
        txn.commit();
    }
    record("tpce", "trade_order", kTxns, t0);
}

void run_wikipedia()
{
    constexpr int kPages = 400;
    constexpr int kUsers = 50;
    constexpr int kOps = 40;
    std::cout << ">> wikipedia\n";
    SqlRunner db(prepare_dir("wikipedia"));
    must(db, "CREATE TABLE useracct (user_id INT PRIMARY KEY, user_name VARCHAR(16))");
    must(db, "CREATE TABLE page (page_id INT PRIMARY KEY, latest INT, page_title VARCHAR(32))");
    must(db, "CREATE TABLE revision (rev_id INT PRIMARY KEY, page_id INT, user_id INT, rev_text VARCHAR(64))");

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> users;
    for (int id = 1; id <= kUsers; ++id)
    {
        users.push_back("(" + std::to_string(id) + "," + q("u" + std::to_string(id)) + ")");
    }
    load_values(db, "INSERT INTO useracct VALUES ", users, 50);
    std::vector<std::string> pages;
    std::vector<std::string> revs;
    for (int id = 1; id <= kPages; ++id)
    {
        pages.push_back("(" + std::to_string(id) + ",0," + q("p" + std::to_string(id)) + ")");
        revs.push_back("(" + std::to_string(id) + "," + std::to_string(id) + ",1," + q(field(64, id)) + ")");
    }
    load_values(db, "INSERT INTO page VALUES ", pages, 50);
    load_values(db, "INSERT INTO revision VALUES ", revs, 40);
    record("wikipedia", "load", kPages, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int page = pick(kPages);
        int user = pick(kUsers);
        int rev = kPages + 1 + i;
        auto txn = db.begin();
        must(txn, "SELECT user_name FROM useracct WHERE user_id = " + std::to_string(user));
        must(txn, "SELECT latest FROM page WHERE page_id = " + std::to_string(page));
        expect_write(
            must(txn, "INSERT INTO revision VALUES (" + std::to_string(rev) + "," + std::to_string(page) + "," + std::to_string(user) + "," + q(field(64, rev)) + ")"),
            "rev");
        expect_write(must(txn, "UPDATE page SET latest = " + std::to_string(rev) + " WHERE page_id = " + std::to_string(page)), "page");
        txn.commit();
    }
    record("wikipedia", "update_page", kOps, t0);
}

void run_auctionmark()
{
    constexpr int kItems = 400;
    constexpr int kOps = 40;
    std::cout << ">> auctionmark\n";
    SqlRunner db(prepare_dir("auctionmark"));
    must(db, "CREATE TABLE item (i_id INT PRIMARY KEY, price INT, num_bids INT, i_name VARCHAR(24))");
    must(db, "CREATE TABLE bid (b_id INT PRIMARY KEY, i_id INT, amount INT)");

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> items;
    for (int id = 1; id <= kItems; ++id)
    {
        items.push_back("(" + std::to_string(id) + ",100,0," + q("item" + std::to_string(id)) + ")");
    }
    load_values(db, "INSERT INTO item VALUES ", items, 50);
    record("auctionmark", "load", kItems, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int item = pick(kItems);
        auto txn = db.begin();
        int64_t price = cell(must(txn, "SELECT price FROM item WHERE i_id = " + std::to_string(item)));
        int64_t bid = price + 1 + (i % 5);
        expect_write(
            must(txn, "UPDATE item SET price = " + std::to_string(bid) + ", num_bids = num_bids + 1 WHERE i_id = " + std::to_string(item)),
            "item");
        expect_write(must(txn, "INSERT INTO bid VALUES (" + std::to_string(i + 1) + "," + std::to_string(item) + "," + std::to_string(bid) + ")"), "bid");
        txn.commit();
    }
    record("auctionmark", "new_bid", kOps, t0);
}



int main(int argc, char ** argv)
{
    try
    {
        std::filesystem::create_directories(kDataRoot);
        std::cout << "engine=sqlite " << sqlite3_libversion() << "\n";
        std::cout << "pragmas=journal_mode=DELETE synchronous=FULL temp_store=MEMORY\n";
        std::cout << "data_dir=" << kDataRoot << "\n";
        std::cout << "filesystem=" << mount_of(kDataRoot) << "\n";
        std::string only = argc > 1 ? argv[1] : "";
        auto want = [&](const char * name) { return only.empty() || only == name; };
        if (want("sysbench"))
            run_sysbench();
        if (want("pgbench"))
            run_pgbench();
        if (want("ycsb"))
            run_ycsb();
        if (want("tpcc"))
            run_tpcc();
        if (want("smallbank"))
            run_smallbank();
        if (want("tatp"))
            run_tatp();
        if (want("tpce"))
            run_tpce();
        if (want("wikipedia"))
            run_wikipedia();
        if (want("auctionmark"))
            run_auctionmark();

        std::cout << "\nsuite        workload                  ops        ms       tps\n";
        for (const auto & s : g_samples)
        {
            double tps = s.ms > 0 ? (s.ops * 1000.0 / s.ms) : 0;
            std::cout << s.suite << "  " << s.workload << "  " << s.ops << "  " << s.ms << "  " << tps << "\n";
        }
        std::cout << "bytes=" << dir_bytes(kDataRoot) << "\n";
        return 0;
    }
    catch (const std::exception & ex)
    {
        std::cerr << "sqlite_oltp failed: " << ex.what() << "\n";
        return 1;
    }
}
