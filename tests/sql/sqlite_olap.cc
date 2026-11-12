#include <sqlite3.h>

#include "base/error.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
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

constexpr const char * kDataRoot = "/home/sehep/github/neon/data/sqlite-olap";
constexpr int kRows = 100000;
constexpr int kOrders = 64;
constexpr int kBatch = 100;
constexpr int kRepeats = 3;

struct Expect
{
    int64_t count = 0;
    int64_t sum_price = 0;
    int64_t sum_expr = 0;
    int64_t sum_filter = 0;
    int64_t min_price = 0;
    int64_t max_price = 0;
    int64_t count_qty = 0;
    int64_t join_rows = 0;
};

Expect expected()
{
    Expect e;
    e.count = kRows;
    e.min_price = 10;
    e.max_price = 59;
    for (int id = 1; id <= kRows; ++id)
    {
        int order_id = (id % kOrders) + 1;
        int quantity = (id % 100) + 1;
        int price = 10 + (id % 50);
        int discount = id % 5;
        e.sum_price += price;
        e.sum_expr += static_cast<int64_t>(price) * quantity;
        if (discount > 2)
        {
            e.sum_filter += price;
        }
        if (quantity > 80)
        {
            e.count_qty++;
        }
        if (order_id % 3 == 1)
        {
            e.join_rows++;
        }
    }
    return e;
}

struct QueryResult
{
    int64_t rows_affected = 0;
    std::vector<std::vector<int64_t>> rows;
};

class Db
{
public:
    Db(Db && other) noexcept : db_(std::exchange(other.db_, nullptr)) {}
    Db(const Db &) = delete;
    Db & operator=(const Db &) = delete;
    Db & operator=(Db &&) = delete;

    static Result<Db> open(const std::string & dir)
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
        Db out(db);
        BENCH_TRY(out.exec("PRAGMA journal_mode=DELETE"));
        BENCH_TRY(out.exec("PRAGMA synchronous=FULL"));
        BENCH_TRY(out.exec("PRAGMA temp_store=MEMORY"));
        BENCH_TRY(out.exec("PRAGMA cache_size=-65536"));
        return out;
    }

    ~Db()
    {
        if (db_ != nullptr)
        {
            sqlite3_close(db_);
        }
    }

    Result<QueryResult> exec(const std::string & sql)
    {
        sqlite3_stmt * stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(stmt, sqlite3_finalize);
        if (rc != SQLITE_OK)
        {
            return popo::base::make_error(rc, std::string("prepare: ") + sqlite3_errmsg(db_) + " :: " + sql);
        }
        QueryResult result;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            int cols = sqlite3_column_count(stmt);
            std::vector<int64_t> row(static_cast<size_t>(cols));
            for (int i = 0; i < cols; ++i)
            {
                row[static_cast<size_t>(i)] = sqlite3_column_int64(stmt, i);
            }
            result.rows.push_back(std::move(row));
        }
        if (rc != SQLITE_DONE)
        {
            return popo::base::make_error(rc, std::string(sqlite3_errmsg(db_)) + " :: " + sql);
        }
        result.rows_affected = sqlite3_changes(db_);
        return result;
    }

private:
    explicit Db(sqlite3 * db) : db_(db) {}
    sqlite3 * db_ = nullptr;
};

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

double elapsed_ms(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

Result<void> load(Db & db)
{
    BENCH_TRY(db.exec("CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, priority INT)"));
    BENCH_TRY(db.exec("CREATE TABLE lineitem (id INT PRIMARY KEY, order_id INT, quantity INT, price INT, discount INT)"));
    std::string orders = "INSERT INTO orders VALUES ";
    for (int id = 1; id <= kOrders; ++id)
    {
        if (id != 1)
        {
            orders.push_back(',');
        }
        orders += "(" + std::to_string(id) + "," + std::to_string(id) + "," + std::to_string(id % 3) + ")";
    }
    BENCH_TRY(db.exec(orders));

    std::string prefix = "INSERT INTO lineitem VALUES ";
    for (int base = 1; base <= kRows; base += kBatch)
    {
        std::string sql = prefix;
        int end = std::min(base + kBatch - 1, kRows);
        for (int id = base; id <= end; ++id)
        {
            if (id != base)
            {
                sql.push_back(',');
            }
            int order_id = (id % kOrders) + 1;
            int quantity = (id % 100) + 1;
            int price = 10 + (id % 50);
            int discount = id % 5;
            sql += "(" + std::to_string(id) + "," + std::to_string(order_id) + "," + std::to_string(quantity) + "," + std::to_string(price) + "," + std::to_string(discount) + ")";
        }
        BENCH_TRY_ASSIGN(wrote, db.exec(sql));
        if (wrote.rows_affected != end - base + 1)
        {
            return popo::base::make_error(Errno::InternalErr, "insert batch wrote " + std::to_string(wrote.rows_affected));
        }
    }
    return {};
}

Result<void> check_scalar(const char * name, int64_t got, int64_t want)
{
    if (got != want)
    {
        return popo::base::make_error(
            Errno::InternalErr, std::string(name) + " got " + std::to_string(got) + " want " + std::to_string(want));
    }
    return {};
}

Result<void> time_query(Db & db, const char * name, const std::string & sql, int64_t want, bool count_rows)
{
    for (int i = 0; i < kRepeats; ++i)
    {
        auto t0 = std::chrono::steady_clock::now();
        BENCH_TRY_ASSIGN(result, db.exec(sql));
        double ms = elapsed_ms(t0);
        int64_t got = 0;
        if (count_rows)
        {
            got = static_cast<int64_t>(result.rows.size());
        }
        else if (result.rows.size() != 1 || result.rows[0].empty())
        {
            return popo::base::make_error(Errno::InternalErr, std::string(name) + " missing scalar");
        }
        else
        {
            got = result.rows[0][0];
        }
        if (i == 0)
        {
            BENCH_TRY(check_scalar(name, got, want));
        }
        double rows_per_s = ms > 0 ? (static_cast<double>(kRows) * 1000.0 / ms) : 0;
        std::cout << "  " << name << "  run=" << (i + 1) << "  ms=" << ms << "  rows/s=" << rows_per_s << "  value=" << got << "\n";
        std::cout.flush();
    }
    return {};
}

Result<void> run()
{
    std::error_code ec;
    std::filesystem::create_directories(kDataRoot, ec);
    if (ec)
        return popo::base::make_error(Errno::IoErr, "create data root: " + ec.message());
    BENCH_TRY_ASSIGN(mount, mount_of(kDataRoot));
    std::cout << "filesystem=" << mount << "\n";
    std::cout << "rows=" << kRows << " orders=" << kOrders << "\n";
    Expect want = expected();

    BENCH_TRY_ASSIGN(dir, prepare_dir("lineitem"));
    BENCH_TRY_ASSIGN(db, Db::open(dir));
    auto t0 = std::chrono::steady_clock::now();
    BENCH_TRY(load(db));
    std::cout << "  load  ms=" << elapsed_ms(t0) << "\n";

    BENCH_TRY(time_query(db, "count_star", "SELECT COUNT(*) FROM lineitem", want.count, false));
    BENCH_TRY(time_query(db, "sum_price", "SELECT SUM(price) FROM lineitem", want.sum_price, false));
    BENCH_TRY(time_query(db, "sum_expr", "SELECT SUM(price * quantity) FROM lineitem", want.sum_expr, false));
    BENCH_TRY(time_query(db, "sum_filter", "SELECT SUM(price) FROM lineitem WHERE discount > 2", want.sum_filter, false));
    BENCH_TRY(time_query(db, "min_price", "SELECT MIN(price) FROM lineitem", want.min_price, false));
    BENCH_TRY(time_query(db, "max_price", "SELECT MAX(price) FROM lineitem", want.max_price, false));
    BENCH_TRY(time_query(db, "count_filter", "SELECT COUNT(*) FROM lineitem WHERE quantity > 80", want.count_qty, false));
    BENCH_TRY(time_query(
        db,
        "join_priority",
        "SELECT lineitem.id FROM lineitem JOIN orders ON lineitem.order_id = orders.id WHERE orders.priority = 1",
        want.join_rows,
        true));
    return {};
}

int main()
{
    auto result = run();
    if (!result)
    {
        std::cerr << "sqlite_olap failed: " << result.error().error_message << "\n";
        return 1;
    }
    return 0;
}
