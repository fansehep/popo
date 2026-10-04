#include "sql_runner.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace
{

constexpr const char * kDataRoot = "/home/sehep/github/neon/data/olap";
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

neon::test::SqlResult must(neon::test::SqlRunner & db, const std::string & sql)
{
    auto result = db.exec(sql);
    if (!result.ok)
    {
        throw std::runtime_error(sql + " :: " + result.error);
    }
    return result;
}

int64_t cell(const neon::test::SqlResult & result, size_t col)
{
    if (result.result.rows.size() != 1 || result.result.rows[0]->values_.size() <= col)
    {
        throw std::runtime_error("expected one scalar row");
    }
    return result.result.rows[0]->values_[col].i64();
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

double elapsed_ms(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void load(neon::test::SqlRunner & db)
{
    must(db, "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, priority INT)");
    must(db, "CREATE TABLE lineitem (id INT PRIMARY KEY, order_id INT, quantity INT, price INT, discount INT)");
    std::string orders = "INSERT INTO orders VALUES ";
    for (int id = 1; id <= kOrders; ++id)
    {
        if (id != 1)
        {
            orders.push_back(',');
        }
        orders += "(" + std::to_string(id) + "," + std::to_string(id) + "," + std::to_string(id % 3) + ")";
    }
    must(db, orders);

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
        auto wrote = must(db, sql);
        if (wrote.result.rows_affected != end - base + 1)
        {
            throw std::runtime_error("insert batch wrote " + std::to_string(wrote.result.rows_affected));
        }
    }
}

void check_scalar(const char * name, int64_t got, int64_t want)
{
    if (got != want)
    {
        throw std::runtime_error(std::string(name) + " got " + std::to_string(got) + " want " + std::to_string(want));
    }
}

void time_query(neon::test::SqlRunner & db, const char * name, const std::string & sql, int64_t want, bool count_rows)
{
    for (int i = 0; i < kRepeats; ++i)
    {
        auto t0 = std::chrono::steady_clock::now();
        auto result = must(db, sql);
        double ms = elapsed_ms(t0);
        int64_t got = count_rows ? static_cast<int64_t>(result.result.rows.size()) : cell(result, 0);
        if (i == 0)
        {
            check_scalar(name, got, want);
        }
        double rows_per_s = ms > 0 ? (static_cast<double>(kRows) * 1000.0 / ms) : 0;
        std::cout << "  " << name << "  run=" << (i + 1) << "  ms=" << ms << "  rows/s=" << rows_per_s << "  value=" << got << "\n";
        std::cout.flush();
    }
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    std::filesystem::create_directories(kDataRoot);
    std::cout << "filesystem=" << mount_of(kDataRoot) << "\n";
    std::cout << "rows=" << kRows << " orders=" << kOrders << "\n";
    Expect want = expected();

    auto dir = prepare_dir("lineitem");
    neon::test::SqlRunner db(dir);
    auto t0 = std::chrono::steady_clock::now();
    load(db);
    std::cout << "  load  ms=" << elapsed_ms(t0) << "\n";

    time_query(db, "count_star", "SELECT COUNT(*) FROM lineitem", want.count, false);
    time_query(db, "sum_price", "SELECT SUM(price) FROM lineitem", want.sum_price, false);
    time_query(db, "sum_expr", "SELECT SUM(price * quantity) FROM lineitem", want.sum_expr, false);
    time_query(db, "sum_filter", "SELECT SUM(price) FROM lineitem WHERE discount > 2", want.sum_filter, false);
    time_query(db, "min_price", "SELECT MIN(price) FROM lineitem", want.min_price, false);
    time_query(db, "max_price", "SELECT MAX(price) FROM lineitem", want.max_price, false);
    time_query(db, "count_filter", "SELECT COUNT(*) FROM lineitem WHERE quantity > 80", want.count_qty, false);
    time_query(db, "join_priority", "SELECT lineitem.id FROM lineitem JOIN orders ON lineitem.order_id = orders.id WHERE orders.priority = 1", want.join_rows, true);
    return 0;
}
