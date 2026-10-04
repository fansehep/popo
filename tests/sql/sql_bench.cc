#include "sql_runner.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace
{

struct Sample
{
    std::string name;
    int64_t ops = 0;
    int64_t rows = 0;
    int64_t total_us = 0;
    int64_t vm_us = 0;
};

int64_t dir_bytes(const std::filesystem::path & dir)
{
    int64_t total = 0;
    for (const auto & entry : std::filesystem::recursive_directory_iterator(dir))
    {
        if (entry.is_regular_file())
        {
            total += static_cast<int64_t>(entry.file_size());
        }
    }
    return total;
}

void print_row(const Sample & sample)
{
    double ms = static_cast<double>(sample.total_us) / 1000.0;
    double per_s = sample.total_us > 0 ? (static_cast<double>(sample.rows) * 1e6) / static_cast<double>(sample.total_us) : 0.0;
    std::cout << sample.name << '\t' << sample.ops << '\t' << sample.rows << '\t' << ms << '\t' << per_s << '\t'
              << (static_cast<double>(sample.vm_us) / 1000.0) << '\n';
}

std::string batch_insert(int begin, int count)
{
    std::string sql = "INSERT INTO bench VALUES ";
    sql.reserve(static_cast<size_t>(count) * 24);
    for (int i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            sql += ", ";
        }
        int id = begin + i;
        sql += "(" + std::to_string(id) + ", 'n" + std::to_string(id) + "')";
    }
    return sql;
}

} // namespace

int main()
{
    constexpr int kBatchRows = 100;
    constexpr int kBatches = 20;
    constexpr int kAutoRows = 100;
    constexpr int kPoints = 50;
    constexpr int kUpdates = 20;

    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("neon_sql_bench_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    spdlog::set_level(spdlog::level::warn);

    neon::test::SqlRunner runner(dir.string());
    std::vector<Sample> samples;

    auto created = runner.exec("CREATE TABLE bench (id INT PRIMARY KEY, name VARCHAR(32))");
    if (!created.ok)
    {
        std::cerr << "create failed: " << created.error << '\n';
        return 1;
    }
    samples.push_back({"create", 1, 0, created.parse_us + created.plan_us + created.compile_us + created.vm_us, created.vm_us});

    Sample batch{"insert_batch_100", kBatches, 0, 0, 0};
    for (int b = 0; b < kBatches; ++b)
    {
        auto result = runner.exec(batch_insert(b * kBatchRows + 1, kBatchRows));
        if (!result.ok)
        {
            std::cerr << "batch insert failed: " << result.error << '\n';
            return 1;
        }
        batch.rows += result.result.rows_affected;
        batch.total_us += result.parse_us + result.plan_us + result.compile_us + result.vm_us;
        batch.vm_us += result.vm_us;
    }
    samples.push_back(batch);

    int auto_base = kBatches * kBatchRows + 1;
    Sample autocommit{"insert_autocommit", kAutoRows, 0, 0, 0};
    for (int i = 0; i < kAutoRows; ++i)
    {
        int id = auto_base + i;
        auto result = runner.exec("INSERT INTO bench VALUES (" + std::to_string(id) + ", 'a" + std::to_string(id) + "')");
        if (!result.ok)
        {
            std::cerr << "autocommit insert failed: " << result.error << '\n';
            return 1;
        }
        autocommit.rows += result.result.rows_affected;
        autocommit.total_us += result.parse_us + result.plan_us + result.compile_us + result.vm_us;
        autocommit.vm_us += result.vm_us;
    }
    samples.push_back(autocommit);

    int expected = kBatches * kBatchRows + kAutoRows;
    auto scanned = runner.exec("SELECT * FROM bench");
    if (!scanned.ok)
    {
        std::cerr << "select * failed: " << scanned.error << '\n';
        return 1;
    }
    samples.push_back(
        {"select_star",
         1,
         static_cast<int64_t>(scanned.result.rows.size()),
         scanned.parse_us + scanned.plan_us + scanned.compile_us + scanned.vm_us,
         scanned.vm_us});
    if (static_cast<int>(scanned.result.rows.size()) != expected)
    {
        std::cerr << "select * returned " << scanned.result.rows.size() << " rows, expected " << expected << '\n';
        return 1;
    }

    Sample points{"select_point", kPoints, 0, 0, 0};
    for (int i = 0; i < kPoints; ++i)
    {
        int id = 1 + (i * 37) % (kBatches * kBatchRows);
        auto result = runner.exec("SELECT name FROM bench WHERE id = " + std::to_string(id));
        if (!result.ok || result.result.rows.size() != 1)
        {
            std::cerr << "point select id=" << id << " failed: " << result.error << " rows=" << result.result.rows.size() << '\n';
            return 1;
        }
        points.rows += static_cast<int64_t>(result.result.rows.size());
        points.total_us += result.parse_us + result.plan_us + result.compile_us + result.vm_us;
        points.vm_us += result.vm_us;
    }
    samples.push_back(points);

    Sample updates{"update_point", kUpdates, 0, 0, 0};
    for (int i = 0; i < kUpdates; ++i)
    {
        int id = 1 + i * 10;
        auto result = runner.exec("UPDATE bench SET name = 'u" + std::to_string(id) + "' WHERE id = " + std::to_string(id));
        if (!result.ok || result.result.rows_affected != 1)
        {
            std::cerr << "update id=" << id << " failed: " << result.error << " affected=" << result.result.rows_affected << '\n';
            return 1;
        }
        updates.rows += result.result.rows_affected;
        updates.total_us += result.parse_us + result.plan_us + result.compile_us + result.vm_us;
        updates.vm_us += result.vm_us;
    }
    samples.push_back(updates);

    Sample deletes{"delete_point", kUpdates, 0, 0, 0};
    for (int i = 0; i < kUpdates; ++i)
    {
        int id = auto_base + i;
        auto result = runner.exec("DELETE FROM bench WHERE id = " + std::to_string(id));
        if (!result.ok || result.result.rows_affected != 1)
        {
            std::cerr << "delete id=" << id << " failed: " << result.error << " affected=" << result.result.rows_affected << '\n';
            return 1;
        }
        deletes.rows += result.result.rows_affected;
        deletes.total_us += result.parse_us + result.plan_us + result.compile_us + result.vm_us;
        deletes.vm_us += result.vm_us;
    }
    samples.push_back(deletes);

    auto counted = runner.exec("SELECT COUNT(*) FROM bench");
    if (!counted.ok || counted.result.rows.size() != 1)
    {
        std::cerr << "count failed: " << counted.error << '\n';
        return 1;
    }
    samples.push_back(
        {"select_count",
         1,
         counted.result.rows[0]->values_[0].i64(),
         counted.parse_us + counted.plan_us + counted.compile_us + counted.vm_us,
         counted.vm_us});

    std::cout << "workload\tops\trows\tms\trows_per_s\tvm_ms\n";
    for (const auto & sample : samples)
    {
        print_row(sample);
    }
    std::cout << "data_bytes\t" << dir_bytes(dir) << '\n';
    return 0;
}
