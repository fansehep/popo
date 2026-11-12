#include "sql_runner.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "src/base/error.h"

namespace
{

constexpr const char * kDataRoot = "/home/sehep/github/neon/data/oltp";

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

popo::base::Result<int64_t> num(const popo::type::Value & v)
{
    switch (v.datatype())
    {
        case popo::type::ValueType::Int8:
            return v.i8();
        case popo::type::ValueType::Int16:
            return v.i16();
        case popo::type::ValueType::Int32:
            return v.i32();
        case popo::type::ValueType::Int64:
            return v.i64();
        case popo::type::ValueType::Uint8:
            return v.u8();
        case popo::type::ValueType::Uint16:
            return v.u16();
        case popo::type::ValueType::Uint32:
            return static_cast<int64_t>(v.u32());
        case popo::type::ValueType::Uint64:
            return static_cast<int64_t>(v.u64());
        default:
            return popo::base::make_error(
                popo::base::InternalErr,
                std::string("expected integer, got ") + popo::type::type_to_string(v.datatype()));
    }
}

popo::base::Result<popo::bytecode::ExecuteSuccess> must(popo::test::SqlRunner & db, const std::string & sql)
{
    auto res = db.exec(sql);
    if (!res)
    {
        return popo::base::make_error(res.error().error_code, sql + "\n" + res.error().error_message);
    }
    return std::move(res->result);
}

popo::base::Result<popo::bytecode::ExecuteSuccess> must(popo::test::SqlTxn & txn, const std::string & sql)
{
    auto res = txn.exec(sql);
    if (!res)
    {
        return popo::base::make_error(res.error().error_code, sql + "\n" + res.error().error_message);
    }
    return std::move(res->result);
}

popo::base::Result<void>
expect_write(popo::base::Result<popo::bytecode::ExecuteSuccess> result, const std::string & sql)
{
    if (!result)
    {
        return std::unexpected(std::move(result).error());
    }
    if (result->rows_affected < 1)
    {
        return popo::base::make_error(popo::base::InternalErr, "no row written: " + sql);
    }
    return {};
}

popo::base::Result<int64_t> cell(popo::base::Result<popo::bytecode::ExecuteSuccess> result, size_t col = 0)
{
    if (!result)
    {
        return std::unexpected(std::move(result).error());
    }
    if (result->rows.empty() || col >= result->rows[0]->values_.size())
    {
        return popo::base::make_error(popo::base::InternalErr, "empty result");
    }
    return num(result->rows[0]->values_[col]);
}

#define BENCH_ASSIGN(name, expression) \
    auto name##_result = (expression); \
    if (!name##_result) \
    { \
        return std::unexpected(std::move(name##_result).error()); \
    } \
    auto name = std::move(*name##_result)

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

popo::base::Result<void>
load_values(popo::test::SqlRunner & db, const std::string & prefix, const std::vector<std::string> & tuples, int batch)
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
        BENCH_ASSIGN(result, must(db, sql));
        if (result.rows_affected != n)
        {
            return popo::base::make_error(
                popo::base::InternalErr,
                "insert batch wrote " + std::to_string(result.rows_affected) + " rows, expected " + std::to_string(n));
        }
    }
    return {};
}

popo::base::Result<std::string> prepare_dir(const std::string & name)
{
    std::filesystem::path dir = std::filesystem::path(kDataRoot) / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::IoErr, "cannot clear " + dir.string() + ": " + ec.message());
    }
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::IoErr, "cannot create " + dir.string() + ": " + ec.message());
    }
    return dir.string();
}

popo::base::Result<std::string> mount_of(const std::string & path)
{
    std::ifstream in("/proc/mounts");
    if (!in)
    {
        return popo::base::make_error(popo::base::IoErr, "cannot read /proc/mounts");
    }
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
        return popo::base::make_error(popo::base::IoErr, "cannot resolve filesystem for " + path);
    }
    if (best_fs == "tmpfs")
    {
        return popo::base::make_error(popo::base::InvalidArg, "refusing tmpfs at " + best_point);
    }
    return best_dev + " " + best_fs + " at " + best_point;
}

uint64_t dir_bytes(const std::filesystem::path & root)
{
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec))
    {
        if (it->is_regular_file(ec))
        {
            n += it->file_size(ec);
        }
    }
    return n;
}

popo::base::Result<void> run_sysbench()
{
    constexpr int kRows = 2000;
    constexpr int kOps = 200;
    std::cout << ">> sysbench\n";
    BENCH_ASSIGN(dir, prepare_dir("sysbench"));
    popo::test::SqlRunner db(dir);
    POPO_TRY(must(db, "CREATE TABLE sbtest1 (id INT PRIMARY KEY, k INT, c VARCHAR(120), pad VARCHAR(60))"));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> tuples;
    tuples.reserve(kRows);
    for (int id = 1; id <= kRows; ++id)
    {
        tuples.push_back(
            "(" + std::to_string(id) + "," + std::to_string(id % 1000) + "," + q(field(120, id)) + ","
            + q(field(60, id + 7)) + ")");
    }
    POPO_TRY(load_values(db, "INSERT INTO sbtest1 VALUES ", tuples, 50));
    record("sysbench", "load", kRows, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        BENCH_ASSIGN(result, must(db, "SELECT c FROM sbtest1 WHERE id = " + std::to_string(id)));
        if (result.rows.size() != 1)
        {
            return popo::base::make_error(popo::base::InternalErr, "sysbench point select missed id");
        }
    }
    record("sysbench", "oltp_point_select", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        std::string sql = "UPDATE sbtest1 SET k = k + 1 WHERE id = " + std::to_string(pick(kRows));
        POPO_TRY(expect_write(must(db, sql), sql));
    }
    record("sysbench", "oltp_update_index", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        std::string sql = "UPDATE sbtest1 SET c = " + q(field(120, 100000 + i)) + " WHERE id = " + std::to_string(id);
        POPO_TRY(expect_write(must(db, sql), sql));
    }
    record("sysbench", "oltp_update_non_index", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = kRows + 1 + i;
        std::string sql = "INSERT INTO sbtest1 VALUES (" + std::to_string(id) + "," + std::to_string(i) + ","
            + q(field(120, id)) + "," + q(field(60, id)) + ")";
        POPO_TRY(expect_write(must(db, sql), sql));
    }
    record("sysbench", "oltp_insert", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = kRows + 1 + i;
        std::string sql = "DELETE FROM sbtest1 WHERE id = " + std::to_string(id);
        POPO_TRY(expect_write(must(db, sql), sql));
    }
    record("sysbench", "oltp_delete", kOps, t0);

    t0 = std::chrono::steady_clock::now();
    constexpr int kRange = 40;
    for (int i = 0; i < kRange; ++i)
    {
        int lo = pick(kRows - 20);
        POPO_TRY(must(
            db, "SELECT c FROM sbtest1 WHERE id >= " + std::to_string(lo) + " AND id <= " + std::to_string(lo + 10)));
    }
    record("sysbench", "oltp_range_select", kRange, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        BENCH_ASSIGN(txn, db.begin());
        for (int s = 0; s < 10; ++s)
        {
            POPO_TRY(must(txn, "SELECT c FROM sbtest1 WHERE id = " + std::to_string(pick(kRows))));
        }
        POPO_TRY(must(
            txn, "SELECT c FROM sbtest1 WHERE id >= " + std::to_string(id) + " AND id <= " + std::to_string(id + 5)));
        POPO_TRY(
            expect_write(must(txn, "UPDATE sbtest1 SET k = k + 1 WHERE id = " + std::to_string(id)), "rw update k"));
        POPO_TRY(expect_write(
            must(txn, "UPDATE sbtest1 SET c = " + q(field(120, 200000 + i)) + " WHERE id = " + std::to_string(id)),
            "rw update c"));
        int fresh = kRows + 10000 + i;
        POPO_TRY(expect_write(
            must(
                txn,
                "INSERT INTO sbtest1 VALUES (" + std::to_string(fresh) + ",1," + q(field(120, fresh)) + ","
                    + q(field(60, fresh)) + ")"),
            "rw insert"));
        POPO_TRY(expect_write(must(txn, "DELETE FROM sbtest1 WHERE id = " + std::to_string(fresh)), "rw delete"));
        POPO_TRY(txn.commit());
    }
    record("sysbench", "oltp_read_write", kOps, t0);
    return {};
}

popo::base::Result<void> run_pgbench()
{
    constexpr int kAccounts = 2000;
    constexpr int kTellers = 10;
    constexpr int kTxns = 200;
    std::cout << ">> pgbench\n";
    BENCH_ASSIGN(dir, prepare_dir("pgbench"));
    popo::test::SqlRunner db(dir);
    POPO_TRY(must(db, "CREATE TABLE pgbench_branches (bid INT PRIMARY KEY, bbalance INT)"));
    POPO_TRY(must(db, "CREATE TABLE pgbench_tellers (tid INT PRIMARY KEY, bid INT, tbalance INT)"));
    POPO_TRY(must(db, "CREATE TABLE pgbench_accounts (aid INT PRIMARY KEY, bid INT, abalance INT)"));
    POPO_TRY(must(db, "CREATE TABLE pgbench_history (hid INT PRIMARY KEY, tid INT, bid INT, aid INT, delta INT)"));

    auto t0 = std::chrono::steady_clock::now();
    POPO_TRY(must(db, "INSERT INTO pgbench_branches VALUES (1,0)"));
    std::vector<std::string> tellers;
    for (int tid = 1; tid <= kTellers; ++tid)
    {
        tellers.push_back("(" + std::to_string(tid) + ",1,0)");
    }
    POPO_TRY(load_values(db, "INSERT INTO pgbench_tellers VALUES ", tellers, 10));
    std::vector<std::string> accounts;
    accounts.reserve(kAccounts);
    for (int aid = 1; aid <= kAccounts; ++aid)
    {
        accounts.push_back("(" + std::to_string(aid) + ",1,0)");
    }
    POPO_TRY(load_values(db, "INSERT INTO pgbench_accounts VALUES ", accounts, 100));
    record("pgbench", "load", kAccounts, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kTxns; ++i)
    {
        int aid = pick(kAccounts);
        int tid = pick(kTellers);
        int delta = (i % 2 == 0) ? 1 : -1;
        BENCH_ASSIGN(txn, db.begin());
        POPO_TRY(expect_write(
            must(
                txn,
                "UPDATE pgbench_accounts SET abalance = abalance + " + std::to_string(delta)
                    + " WHERE aid = " + std::to_string(aid)),
            "account"));
        POPO_TRY(must(txn, "SELECT abalance FROM pgbench_accounts WHERE aid = " + std::to_string(aid)));
        POPO_TRY(expect_write(
            must(
                txn,
                "UPDATE pgbench_tellers SET tbalance = tbalance + " + std::to_string(delta)
                    + " WHERE tid = " + std::to_string(tid)),
            "teller"));
        POPO_TRY(expect_write(
            must(txn, "UPDATE pgbench_branches SET bbalance = bbalance + " + std::to_string(delta) + " WHERE bid = 1"),
            "branch"));
        POPO_TRY(expect_write(
            must(
                txn,
                "INSERT INTO pgbench_history VALUES (" + std::to_string(i + 1) + "," + std::to_string(tid) + ",1,"
                    + std::to_string(aid) + "," + std::to_string(delta) + ")"),
            "history"));
        POPO_TRY(txn.commit());
    }
    record("pgbench", "tpcb", kTxns, t0);
    return {};
}

popo::base::Result<void> run_ycsb()
{
    constexpr int kRows = 1000;
    constexpr int kOps = 200;
    constexpr int kFields = 10;
    constexpr int kWidth = 100;
    std::cout << ">> ycsb\n";
    BENCH_ASSIGN(dir, prepare_dir("ycsb"));
    popo::test::SqlRunner db(dir);
    std::string ddl = "CREATE TABLE usertable (id INT PRIMARY KEY";
    for (int f = 0; f < kFields; ++f)
    {
        ddl += ", f" + std::to_string(f) + " VARCHAR(" + std::to_string(kWidth) + ")";
    }
    ddl += ")";
    POPO_TRY(must(db, ddl));

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
    POPO_TRY(load_values(db, "INSERT INTO usertable VALUES ", tuples, 20));
    record("ycsb", "load", kRows, t0);

    auto mix = [&](const char * name, int reads_per_100) -> popo::base::Result<void>
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i)
        {
            int id = pick(kRows);
            bool read = (i % 100) < reads_per_100;
            if (read)
            {
                BENCH_ASSIGN(result, must(db, "SELECT f0 FROM usertable WHERE id = " + std::to_string(id)));
                if (result.rows.size() != 1)
                {
                    return popo::base::make_error(popo::base::InternalErr, "ycsb read missed");
                }
            }
            else
            {
                std::string sql
                    = "UPDATE usertable SET f0 = " + q(field(kWidth, 500000 + i)) + " WHERE id = " + std::to_string(id);
                POPO_TRY(expect_write(must(db, sql), sql));
            }
        }
        record("ycsb", name, kOps, start);
        return {};
    };
    POPO_TRY(mix("workload_a", 50));
    POPO_TRY(mix("workload_b", 95));
    POPO_TRY(mix("workload_c", 100));

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int id = pick(kRows);
        BENCH_ASSIGN(txn, db.begin());
        BENCH_ASSIGN(result, must(txn, "SELECT f0 FROM usertable WHERE id = " + std::to_string(id)));
        if (result.rows.size() != 1)
        {
            return popo::base::make_error(popo::base::InternalErr, "ycsb rmw missed");
        }
        POPO_TRY(expect_write(
            must(
                txn, "UPDATE usertable SET f0 = " + q(field(kWidth, 800000 + i)) + " WHERE id = " + std::to_string(id)),
            "ycsb rmw"));
        POPO_TRY(txn.commit());
    }
    record("ycsb", "workload_f", kOps, start);
    return {};
}

popo::base::Result<void> run_tpcc()
{
    constexpr int kDistricts = 4;
    constexpr int kCustomersPer = 50;
    constexpr int kItems = 200;
    constexpr int kNewOrder = 30;
    constexpr int kPayment = 30;
    constexpr int kCustomers = kDistricts * kCustomersPer;
    std::cout << ">> tpcc\n";
    BENCH_ASSIGN(dir, prepare_dir("tpcc"));
    popo::test::SqlRunner db(dir);
    POPO_TRY(must(db, "CREATE TABLE warehouse (w_id INT PRIMARY KEY, w_ytd INT, w_name VARCHAR(16))"));
    POPO_TRY(must(db, "CREATE TABLE district (d_id INT PRIMARY KEY, d_ytd INT, d_next_o_id INT, d_name VARCHAR(16))"));
    POPO_TRY(must(db, "CREATE TABLE customer (c_id INT PRIMARY KEY, c_d_id INT, c_balance INT, c_last VARCHAR(16))"));
    POPO_TRY(must(db, "CREATE TABLE item (i_id INT PRIMARY KEY, i_price INT, i_name VARCHAR(24))"));
    POPO_TRY(must(db, "CREATE TABLE stock (s_id INT PRIMARY KEY, s_quantity INT, s_order_cnt INT)"));
    POPO_TRY(must(db, "CREATE TABLE orders (o_id INT PRIMARY KEY, o_d_id INT, o_c_id INT, o_ol_cnt INT)"));
    POPO_TRY(must(db, "CREATE TABLE new_order (no_id INT PRIMARY KEY, no_d_id INT, no_c_id INT)"));
    POPO_TRY(must(db, "CREATE TABLE order_line (ol_id INT PRIMARY KEY, ol_o_id INT, ol_i_id INT, ol_amount INT)"));
    POPO_TRY(must(db, "CREATE INDEX ol_o_id_idx ON order_line (ol_o_id)"));
    POPO_TRY(must(db, "CREATE TABLE history (h_id INT PRIMARY KEY, h_c_id INT, h_d_id INT, h_amount INT)"));

    auto t0 = std::chrono::steady_clock::now();
    POPO_TRY(must(db, "INSERT INTO warehouse VALUES (1,0,'w1')"));
    std::vector<std::string> rows;
    for (int d = 1; d <= kDistricts; ++d)
    {
        rows.push_back("(" + std::to_string(d) + ",0,1," + q("d" + std::to_string(d)) + ")");
    }
    POPO_TRY(load_values(db, "INSERT INTO district VALUES ", rows, 10));
    rows.clear();
    for (int c = 1; c <= kCustomers; ++c)
    {
        int d = ((c - 1) / kCustomersPer) + 1;
        rows.push_back("(" + std::to_string(c) + "," + std::to_string(d) + ",1000," + q("c" + std::to_string(c)) + ")");
    }
    POPO_TRY(load_values(db, "INSERT INTO customer VALUES ", rows, 50));
    rows.clear();
    for (int i = 1; i <= kItems; ++i)
    {
        rows.push_back(
            "(" + std::to_string(i) + "," + std::to_string(10 + (i % 50)) + "," + q("item" + std::to_string(i)) + ")");
    }
    POPO_TRY(load_values(db, "INSERT INTO item VALUES ", rows, 50));
    rows.clear();
    for (int i = 1; i <= kItems; ++i)
    {
        rows.push_back("(" + std::to_string(i) + ",50,0)");
    }
    POPO_TRY(load_values(db, "INSERT INTO stock VALUES ", rows, 50));
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
        BENCH_ASSIGN(txn, db.begin());
        POPO_TRY(must(txn, "SELECT w_ytd FROM warehouse WHERE w_id = 1"));
        BENCH_ASSIGN(seq, cell(must(txn, "SELECT d_next_o_id FROM district WHERE d_id = " + std::to_string(d))));
        int64_t oid = seq * 100 + d;
        POPO_TRY(expect_write(
            must(txn, "UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = " + std::to_string(d)),
            "district"));
        POPO_TRY(expect_write(
            must(
                txn,
                "INSERT INTO orders VALUES (" + std::to_string(oid) + "," + std::to_string(d) + "," + std::to_string(c)
                    + "," + std::to_string(kLines) + ")"),
            "orders"));
        POPO_TRY(expect_write(
            must(
                txn,
                "INSERT INTO new_order VALUES (" + std::to_string(oid) + "," + std::to_string(d) + ","
                    + std::to_string(c) + ")"),
            "new_order"));
        for (int line = 0; line < kLines; ++line)
        {
            int item = pick(kItems);
            BENCH_ASSIGN(price, cell(must(txn, "SELECT i_price FROM item WHERE i_id = " + std::to_string(item))));
            BENCH_ASSIGN(qty, cell(must(txn, "SELECT s_quantity FROM stock WHERE s_id = " + std::to_string(item))));
            int64_t next_qty = qty - 5;
            if (next_qty < 10)
            {
                next_qty += 91;
            }
            POPO_TRY(expect_write(
                must(
                    txn,
                    "UPDATE stock SET s_quantity = " + std::to_string(next_qty)
                        + ", s_order_cnt = s_order_cnt + 1 WHERE s_id = " + std::to_string(item)),
                "stock"));
            int ol = next_line++;
            POPO_TRY(expect_write(
                must(
                    txn,
                    "INSERT INTO order_line VALUES (" + std::to_string(ol) + "," + std::to_string(oid) + ","
                        + std::to_string(item) + "," + std::to_string(price * 5) + ")"),
                "order_line"));
        }
        POPO_TRY(txn.commit());
        live_orders.push_back(static_cast<int>(oid));
    }
    record("tpcc", "new_order", kNewOrder, t0);

    t0 = std::chrono::steady_clock::now();
    for (int n = 0; n < kPayment; ++n)
    {
        int d = pick(kDistricts);
        int c = (d - 1) * kCustomersPer + pick(kCustomersPer);
        int amount = 5 + (n % 20);
        BENCH_ASSIGN(txn, db.begin());
        POPO_TRY(expect_write(
            must(txn, "UPDATE warehouse SET w_ytd = w_ytd + " + std::to_string(amount) + " WHERE w_id = 1"), "w"));
        POPO_TRY(expect_write(
            must(
                txn,
                "UPDATE district SET d_ytd = d_ytd + " + std::to_string(amount) + " WHERE d_id = " + std::to_string(d)),
            "d"));
        POPO_TRY(must(txn, "SELECT c_balance, c_last FROM customer WHERE c_id = " + std::to_string(c)));
        POPO_TRY(expect_write(
            must(
                txn,
                "UPDATE customer SET c_balance = c_balance - " + std::to_string(amount)
                    + " WHERE c_id = " + std::to_string(c)),
            "c"));
        POPO_TRY(expect_write(
            must(
                txn,
                "INSERT INTO history VALUES (" + std::to_string(next_hist++) + "," + std::to_string(c) + ","
                    + std::to_string(d) + "," + std::to_string(amount) + ")"),
            "h"));
        POPO_TRY(txn.commit());
    }
    record("tpcc", "payment", kPayment, t0);

    t0 = std::chrono::steady_clock::now();
    int status_ops = static_cast<int>(live_orders.size());
    for (int oid : live_orders)
    {
        BENCH_ASSIGN(txn, db.begin());
        POPO_TRY(must(txn, "SELECT o_c_id, o_ol_cnt FROM orders WHERE o_id = " + std::to_string(oid)));
        POPO_TRY(must(txn, "SELECT ol_i_id, ol_amount FROM order_line WHERE ol_o_id = " + std::to_string(oid)));
        POPO_TRY(txn.commit());
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
        BENCH_ASSIGN(txn, db.begin());
        BENCH_ASSIGN(cid, cell(must(txn, "SELECT no_c_id FROM new_order WHERE no_id = " + std::to_string(oid))));
        POPO_TRY(
            expect_write(must(txn, "DELETE FROM new_order WHERE no_id = " + std::to_string(oid)), "delivery delete"));
        POPO_TRY(must(txn, "SELECT ol_amount FROM order_line WHERE ol_o_id = " + std::to_string(oid)));
        POPO_TRY(expect_write(
            must(txn, "UPDATE customer SET c_balance = c_balance + 10 WHERE c_id = " + std::to_string(cid)),
            "delivery customer"));
        POPO_TRY(txn.commit());
        ++delivered;
    }
    record("tpcc", "delivery", delivered, t0);

    t0 = std::chrono::steady_clock::now();
    constexpr int kStockLevel = 20;
    for (int n = 0; n < kStockLevel; ++n)
    {
        BENCH_ASSIGN(txn, db.begin());
        int low = 0;
        for (int i = 0; i < 20; ++i)
        {
            BENCH_ASSIGN(
                qty, cell(must(txn, "SELECT s_quantity FROM stock WHERE s_id = " + std::to_string(pick(kItems)))));
            if (qty < 20)
            {
                ++low;
            }
        }
        (void)low;
        POPO_TRY(txn.commit());
    }
    record("tpcc", "stock_level", kStockLevel, t0);
    return {};
}

popo::base::Result<void> run_smallbank()
{
    constexpr int kCustomers = 1000;
    constexpr int kOps = 80;
    std::cout << ">> smallbank\n";
    BENCH_ASSIGN(dir, prepare_dir("smallbank"));
    popo::test::SqlRunner db(dir);
    POPO_TRY(must(db, "CREATE TABLE accounts (custid INT PRIMARY KEY, name VARCHAR(16))"));
    POPO_TRY(must(db, "CREATE TABLE savings (custid INT PRIMARY KEY, bal INT)"));
    POPO_TRY(must(db, "CREATE TABLE checking (custid INT PRIMARY KEY, bal INT)"));

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
    POPO_TRY(load_values(db, "INSERT INTO accounts VALUES ", accounts, 100));
    POPO_TRY(load_values(db, "INSERT INTO savings VALUES ", savings, 100));
    POPO_TRY(load_values(db, "INSERT INTO checking VALUES ", checking, 100));
    record("smallbank", "load", kCustomers, t0);

    auto one = [&](const char * name, auto && body) -> popo::base::Result<void>
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i)
        {
            POPO_TRY(body(i));
        }
        record("smallbank", name, kOps, start);
        return {};
    };

    POPO_TRY(one(
        "amalgamate",
        [&](int) -> popo::base::Result<void>
        {
            int a = pick(kCustomers);
            int b = pick(kCustomers);
            BENCH_ASSIGN(txn, db.begin());
            BENCH_ASSIGN(sav, cell(must(txn, "SELECT bal FROM savings WHERE custid = " + std::to_string(a))));
            BENCH_ASSIGN(chk, cell(must(txn, "SELECT bal FROM checking WHERE custid = " + std::to_string(a))));
            POPO_TRY(expect_write(must(txn, "UPDATE savings SET bal = 0 WHERE custid = " + std::to_string(a)), "sav"));
            POPO_TRY(expect_write(must(txn, "UPDATE checking SET bal = 0 WHERE custid = " + std::to_string(a)), "chk"));
            POPO_TRY(expect_write(
                must(
                    txn,
                    "UPDATE checking SET bal = bal + " + std::to_string(sav + chk)
                        + " WHERE custid = " + std::to_string(b)),
                "dest"));
            POPO_TRY(txn.commit());
            return {};
        }));
    POPO_TRY(
        one("balance",
            [&](int) -> popo::base::Result<void>
            {
                int id = pick(kCustomers);
                BENCH_ASSIGN(txn, db.begin());
                POPO_TRY(cell(must(txn, "SELECT bal FROM savings WHERE custid = " + std::to_string(id))));
                POPO_TRY(cell(must(txn, "SELECT bal FROM checking WHERE custid = " + std::to_string(id))));
                POPO_TRY(txn.commit());
                return {};
            }));
    POPO_TRY(
        one("deposit_checking",
            [&](int) -> popo::base::Result<void>
            {
                int id = pick(kCustomers);
                BENCH_ASSIGN(txn, db.begin());
                POPO_TRY(expect_write(
                    must(txn, "UPDATE checking SET bal = bal + 10 WHERE custid = " + std::to_string(id)), "dep"));
                POPO_TRY(txn.commit());
                return {};
            }));
    POPO_TRY(
        one("transact_saving",
            [&](int) -> popo::base::Result<void>
            {
                int id = pick(kCustomers);
                BENCH_ASSIGN(txn, db.begin());
                POPO_TRY(expect_write(
                    must(txn, "UPDATE savings SET bal = bal + 10 WHERE custid = " + std::to_string(id)), "sav"));
                POPO_TRY(txn.commit());
                return {};
            }));
    POPO_TRY(
        one("write_check",
            [&](int) -> popo::base::Result<void>
            {
                int id = pick(kCustomers);
                BENCH_ASSIGN(txn, db.begin());
                BENCH_ASSIGN(total, cell(must(txn, "SELECT bal FROM savings WHERE custid = " + std::to_string(id))));
                BENCH_ASSIGN(
                    checking_balance, cell(must(txn, "SELECT bal FROM checking WHERE custid = " + std::to_string(id))));
                total += checking_balance;
                (void)total;
                POPO_TRY(expect_write(
                    must(txn, "UPDATE checking SET bal = bal - 5 WHERE custid = " + std::to_string(id)), "check"));
                POPO_TRY(txn.commit());
                return {};
            }));
    return {};
}

popo::base::Result<void> run_tatp()
{
    constexpr int kSubs = 1000;
    constexpr int kOps = 60;
    std::cout << ">> tatp\n";
    BENCH_ASSIGN(dir, prepare_dir("tatp"));
    popo::test::SqlRunner db(dir);
    POPO_TRY(must(
        db,
        "CREATE TABLE subscriber (s_id INT PRIMARY KEY, sub_nbr VARCHAR(15), bit_1 INT, hex_1 INT, vlr_location INT)"));
    POPO_TRY(must(db, "CREATE TABLE access_info (s_id INT PRIMARY KEY, data1 INT, data2 INT)"));
    POPO_TRY(must(db, "CREATE TABLE special_facility (s_id INT PRIMARY KEY, sf_type INT, is_active INT)"));
    POPO_TRY(must(
        db, "CREATE TABLE call_forwarding (s_id INT PRIMARY KEY, time_start INT, time_end INT, numberx VARCHAR(15))"));

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
    POPO_TRY(load_values(db, "INSERT INTO subscriber VALUES ", subs, 50));
    POPO_TRY(load_values(db, "INSERT INTO access_info VALUES ", access, 100));
    POPO_TRY(load_values(db, "INSERT INTO special_facility VALUES ", facility, 100));
    POPO_TRY(load_values(db, "INSERT INTO call_forwarding VALUES ", forward, 50));
    record("tatp", "load", kSubs, t0);

    auto timed = [&](const char * name, auto && body) -> popo::base::Result<void>
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i)
        {
            POPO_TRY(body(i));
        }
        record("tatp", name, kOps, start);
        return {};
    };
    POPO_TRY(timed(
        "get_subscriber",
        [&](int) -> popo::base::Result<void>
        {
            BENCH_ASSIGN(
                result,
                must(db, "SELECT sub_nbr, vlr_location FROM subscriber WHERE s_id = " + std::to_string(pick(kSubs))));
            if (result.rows.size() != 1)
            {
                return popo::base::make_error(popo::base::InternalErr, "tatp get_subscriber");
            }
            return {};
        }));
    POPO_TRY(timed(
        "get_new_destination",
        [&](int) -> popo::base::Result<void>
        {
            BENCH_ASSIGN(
                result, must(db, "SELECT numberx FROM call_forwarding WHERE s_id = " + std::to_string(pick(kSubs))));
            if (result.rows.size() != 1)
            {
                return popo::base::make_error(popo::base::InternalErr, "tatp get_new_destination");
            }
            return {};
        }));
    POPO_TRY(timed(
        "get_access_data",
        [&](int) -> popo::base::Result<void>
        {
            POPO_TRY(cell(must(db, "SELECT data1 FROM access_info WHERE s_id = " + std::to_string(pick(kSubs)))));
            return {};
        }));
    POPO_TRY(timed(
        "update_subscriber",
        [&](int i) -> popo::base::Result<void>
        {
            std::string sql = "UPDATE subscriber SET bit_1 = " + std::to_string(i % 2)
                + ", hex_1 = " + std::to_string((i % 16) + 1) + " WHERE s_id = " + std::to_string(pick(kSubs));
            POPO_TRY(expect_write(must(db, sql), sql));
            return {};
        }));
    POPO_TRY(timed(
        "update_location",
        [&](int i) -> popo::base::Result<void>
        {
            std::string sql = "UPDATE subscriber SET vlr_location = " + std::to_string(1000 + i)
                + " WHERE s_id = " + std::to_string(pick(kSubs));
            POPO_TRY(expect_write(must(db, sql), sql));
            return {};
        }));
    POPO_TRY(timed(
        "insert_call_forwarding",
        [&](int i) -> popo::base::Result<void>
        {
            int id = kSubs + 1 + i;
            BENCH_ASSIGN(txn, db.begin());
            POPO_TRY(expect_write(
                must(txn, "INSERT INTO subscriber VALUES (" + std::to_string(id) + "," + q(field(15, id)) + ",1,1,1)"),
                "sub"));
            POPO_TRY(expect_write(
                must(
                    txn,
                    "INSERT INTO call_forwarding VALUES (" + std::to_string(id) + ",8,16," + q(field(15, id)) + ")"),
                "cf"));
            POPO_TRY(txn.commit());
            return {};
        }));
    POPO_TRY(timed(
        "delete_call_forwarding",
        [&](int i) -> popo::base::Result<void>
        {
            int id = kSubs + 1 + i;
            std::string sql = "DELETE FROM call_forwarding WHERE s_id = " + std::to_string(id);
            POPO_TRY(expect_write(must(db, sql), sql));
            return {};
        }));
    return {};
}

popo::base::Result<void> run_tpce()
{
    constexpr int kAccounts = 500;
    constexpr int kSecurities = 100;
    constexpr int kTxns = 40;
    std::cout << ">> tpce\n";
    BENCH_ASSIGN(dir, prepare_dir("tpce"));
    popo::test::SqlRunner db(dir);
    POPO_TRY(must(db, "CREATE TABLE customer_account (ca_id INT PRIMARY KEY, balance INT)"));
    POPO_TRY(must(db, "CREATE TABLE security (s_id INT PRIMARY KEY, price INT)"));
    POPO_TRY(must(db, "CREATE TABLE holding (h_id INT PRIMARY KEY, qty INT)"));
    POPO_TRY(must(db, "CREATE TABLE trade (t_id INT PRIMARY KEY, ca_id INT, s_id INT, qty INT, price INT)"));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> accounts;
    std::vector<std::string> holdings;
    for (int id = 1; id <= kAccounts; ++id)
    {
        accounts.push_back("(" + std::to_string(id) + ",1000000)");
        holdings.push_back("(" + std::to_string(id) + ",100)");
    }
    POPO_TRY(load_values(db, "INSERT INTO customer_account VALUES ", accounts, 100));
    POPO_TRY(load_values(db, "INSERT INTO holding VALUES ", holdings, 100));
    std::vector<std::string> secs;
    for (int id = 1; id <= kSecurities; ++id)
    {
        secs.push_back("(" + std::to_string(id) + "," + std::to_string(20 + id) + ")");
    }
    POPO_TRY(load_values(db, "INSERT INTO security VALUES ", secs, 50));
    record("tpce", "load", kAccounts, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kTxns; ++i)
    {
        int ca = pick(kAccounts);
        int sec = pick(kSecurities);
        BENCH_ASSIGN(txn, db.begin());
        POPO_TRY(cell(must(txn, "SELECT balance FROM customer_account WHERE ca_id = " + std::to_string(ca))));
        BENCH_ASSIGN(price, cell(must(txn, "SELECT price FROM security WHERE s_id = " + std::to_string(sec))));
        POPO_TRY(
            expect_write(must(txn, "UPDATE holding SET qty = qty + 1 WHERE h_id = " + std::to_string(ca)), "holding"));
        POPO_TRY(expect_write(
            must(
                txn,
                "UPDATE customer_account SET balance = balance - " + std::to_string(price)
                    + " WHERE ca_id = " + std::to_string(ca)),
            "cash"));
        POPO_TRY(expect_write(
            must(
                txn,
                "INSERT INTO trade VALUES (" + std::to_string(i + 1) + "," + std::to_string(ca) + ","
                    + std::to_string(sec) + ",1," + std::to_string(price) + ")"),
            "trade"));
        POPO_TRY(txn.commit());
    }
    record("tpce", "trade_order", kTxns, t0);
    return {};
}

popo::base::Result<void> run_wikipedia()
{
    constexpr int kPages = 400;
    constexpr int kUsers = 50;
    constexpr int kOps = 40;
    std::cout << ">> wikipedia\n";
    BENCH_ASSIGN(dir, prepare_dir("wikipedia"));
    popo::test::SqlRunner db(dir);
    POPO_TRY(must(db, "CREATE TABLE useracct (user_id INT PRIMARY KEY, user_name VARCHAR(16))"));
    POPO_TRY(must(db, "CREATE TABLE page (page_id INT PRIMARY KEY, latest INT, page_title VARCHAR(32))"));
    POPO_TRY(
        must(db, "CREATE TABLE revision (rev_id INT PRIMARY KEY, page_id INT, user_id INT, rev_text VARCHAR(64))"));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> users;
    for (int id = 1; id <= kUsers; ++id)
    {
        users.push_back("(" + std::to_string(id) + "," + q("u" + std::to_string(id)) + ")");
    }
    POPO_TRY(load_values(db, "INSERT INTO useracct VALUES ", users, 50));
    std::vector<std::string> pages;
    std::vector<std::string> revs;
    for (int id = 1; id <= kPages; ++id)
    {
        pages.push_back("(" + std::to_string(id) + ",0," + q("p" + std::to_string(id)) + ")");
        revs.push_back("(" + std::to_string(id) + "," + std::to_string(id) + ",1," + q(field(64, id)) + ")");
    }
    POPO_TRY(load_values(db, "INSERT INTO page VALUES ", pages, 50));
    POPO_TRY(load_values(db, "INSERT INTO revision VALUES ", revs, 40));
    record("wikipedia", "load", kPages, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int page = pick(kPages);
        int user = pick(kUsers);
        int rev = kPages + 1 + i;
        BENCH_ASSIGN(txn, db.begin());
        POPO_TRY(must(txn, "SELECT user_name FROM useracct WHERE user_id = " + std::to_string(user)));
        POPO_TRY(must(txn, "SELECT latest FROM page WHERE page_id = " + std::to_string(page)));
        POPO_TRY(expect_write(
            must(
                txn,
                "INSERT INTO revision VALUES (" + std::to_string(rev) + "," + std::to_string(page) + ","
                    + std::to_string(user) + "," + q(field(64, rev)) + ")"),
            "rev"));
        POPO_TRY(expect_write(
            must(txn, "UPDATE page SET latest = " + std::to_string(rev) + " WHERE page_id = " + std::to_string(page)),
            "page"));
        POPO_TRY(txn.commit());
    }
    record("wikipedia", "update_page", kOps, t0);
    return {};
}

popo::base::Result<void> run_auctionmark()
{
    constexpr int kItems = 400;
    constexpr int kOps = 40;
    std::cout << ">> auctionmark\n";
    BENCH_ASSIGN(dir, prepare_dir("auctionmark"));
    popo::test::SqlRunner db(dir);
    POPO_TRY(must(db, "CREATE TABLE item (i_id INT PRIMARY KEY, price INT, num_bids INT, i_name VARCHAR(24))"));
    POPO_TRY(must(db, "CREATE TABLE bid (b_id INT PRIMARY KEY, i_id INT, amount INT)"));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> items;
    for (int id = 1; id <= kItems; ++id)
    {
        items.push_back("(" + std::to_string(id) + ",100,0," + q("item" + std::to_string(id)) + ")");
    }
    POPO_TRY(load_values(db, "INSERT INTO item VALUES ", items, 50));
    record("auctionmark", "load", kItems, t0);

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i)
    {
        int item = pick(kItems);
        BENCH_ASSIGN(txn, db.begin());
        BENCH_ASSIGN(price, cell(must(txn, "SELECT price FROM item WHERE i_id = " + std::to_string(item))));
        int64_t bid = price + 1 + (i % 5);
        POPO_TRY(expect_write(
            must(
                txn,
                "UPDATE item SET price = " + std::to_string(bid)
                    + ", num_bids = num_bids + 1 WHERE i_id = " + std::to_string(item)),
            "item"));
        POPO_TRY(expect_write(
            must(
                txn,
                "INSERT INTO bid VALUES (" + std::to_string(i + 1) + "," + std::to_string(item) + ","
                    + std::to_string(bid) + ")"),
            "bid"));
        POPO_TRY(txn.commit());
    }
    record("auctionmark", "new_bid", kOps, t0);
    return {};
}

popo::base::Result<void> run(int argc, char ** argv)
{
    std::error_code ec;
    std::filesystem::create_directories(kDataRoot, ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::IoErr, std::string("cannot create data root: ") + ec.message());
    }
    BENCH_ASSIGN(mount, mount_of(kDataRoot));
    std::cout << "data_dir=" << kDataRoot << "\n";
    std::cout << "filesystem=" << mount << "\n";
    std::string only = argc > 1 ? argv[1] : "";
    auto want = [&](const char * name) { return only.empty() || only == name; };
    if (want("sysbench"))
        POPO_TRY(run_sysbench());
    if (want("pgbench"))
        POPO_TRY(run_pgbench());
    if (want("ycsb"))
        POPO_TRY(run_ycsb());
    if (want("tpcc"))
        POPO_TRY(run_tpcc());
    if (want("smallbank"))
        POPO_TRY(run_smallbank());
    if (want("tatp"))
        POPO_TRY(run_tatp());
    if (want("tpce"))
        POPO_TRY(run_tpce());
    if (want("wikipedia"))
        POPO_TRY(run_wikipedia());
    if (want("auctionmark"))
        POPO_TRY(run_auctionmark());

    std::cout << "\nsuite        workload                  ops        ms       tps\n";
    for (const auto & s : g_samples)
    {
        double tps = s.ms > 0 ? (s.ops * 1000.0 / s.ms) : 0;
        std::cout << s.suite << "  " << s.workload << "  " << s.ops << "  " << s.ms << "  " << tps << "\n";
    }
    std::cout << "bytes=" << dir_bytes(kDataRoot) << "\n";
    return {};
}

} // namespace

int main(int argc, char ** argv)
{
    spdlog::set_level(spdlog::level::warn);
    auto result = run(argc, argv);
    if (!result)
    {
        std::cerr << "oltp_bench failed: " << result.error().error_message << "\n";
        return 1;
    }
    return 0;
}
