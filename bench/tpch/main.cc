#include "bench/tpch/workload.h"
#include "tests/sql/sql_runner.h"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <thread>

namespace
{

using popo::base::Errno;
using popo::base::Result;
using Clock = std::chrono::steady_clock;

struct Options
{
    popo::tpch::GeneratorConfig generator;
    std::filesystem::path database_dir;
    std::filesystem::path result_dir;
    std::string engine = "popodb";
    std::string phase = "all";
    bool resume = false;
    bool force = false;
    size_t batch_rows = 1000;
    size_t executor_memory_bytes = 4ULL * 1024 * 1024 * 1024;
    size_t buffer_pool_pages = (2ULL * 1024 * 1024 * 1024) / 4096;
    std::filesystem::path temp_dir;
    std::array<bool, 23> skip{};
};

struct Cell
{
    enum class Kind
    {
        Null,
        Number,
        Text
    };
    Kind kind = Kind::Null;
    long double number = 0;
    std::string text;
};

struct QuerySample
{
    size_t query = 0;
    double milliseconds = 0;
    size_t rows = 0;
    uint64_t peak_rss_bytes = 0;
    uint64_t executor_peak_bytes = 0;
    uint64_t spill_bytes = 0;
    size_t spill_files = 0;
    std::string digest;
    std::vector<std::vector<Cell>> result;
    std::string status = "ok";
    std::string error;
};

#define TPCH_TRY(expr) \
    do \
    { \
        auto result = (expr); \
        if (!result) \
            return std::unexpected(std::move(result.error())); \
    } while (false)

std::string json_quote(std::string_view value)
{
    std::string out = "\"";
    for (char c : value)
    {
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(c);
        }
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else
            out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string csv_quote(std::string_view value)
{
    std::string out = "\"";
    for (char c : value)
    {
        out.push_back(c);
        if (c == '"')
            out.push_back('"');
    }
    out.push_back('"');
    return out;
}

uint64_t current_rss_bytes()
{
    std::ifstream status("/proc/self/status");
    std::string key;
    uint64_t value = 0;
    std::string unit;
    while (status >> key >> value >> unit)
    {
        if (key == "VmRSS:")
            return value * 1024;
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 0;
}

class RssMonitor
{
public:
    RssMonitor() : peak_(current_rss_bytes())
    {
        thread_ = std::thread(
            [this]
            {
                while (!stop_.load(std::memory_order_relaxed))
                {
                    uint64_t value = current_rss_bytes();
                    uint64_t peak = peak_.load(std::memory_order_relaxed);
                    while (value > peak && !peak_.compare_exchange_weak(peak, value, std::memory_order_relaxed))
                    {
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            });
    }

    ~RssMonitor()
    {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable())
            thread_.join();
    }

    uint64_t finish()
    {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable())
            thread_.join();
        return std::max(peak_.load(std::memory_order_relaxed), current_rss_bytes());
    }

private:
    std::atomic<bool> stop_ = false;
    std::atomic<uint64_t> peak_ = 0;
    std::thread thread_;
};

std::string canonical_cell(const Cell & cell)
{
    if (cell.kind == Cell::Kind::Null)
        return "Z";
    if (cell.kind == Cell::Kind::Text)
        return "T" + std::to_string(cell.text.size()) + ":" + cell.text;
    std::ostringstream out;
    // TPC-H comparisons use 1e-6 absolute / 1e-8 relative tolerance. Nine
    // significant decimal digits make the recorded digest stable at that
    // tolerance; exact tolerance checking is still performed cell by cell.
    out << "N" << std::scientific << std::setprecision(8) << cell.number;
    return out.str();
}

std::string result_digest(const std::vector<std::vector<Cell>> & rows)
{
    uint64_t hash = 1469598103934665603ULL;
    auto add = [&](std::string_view value)
    {
        for (unsigned char c : value)
        {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xff;
        hash *= 1099511628211ULL;
    };
    for (const auto & row : rows)
    {
        for (const auto & cell : row)
            add(canonical_cell(cell));
        add("\n");
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

Cell popodb_cell(const popo::type::Value & value)
{
    if (value.datatype() == popo::type::ValueType::Null)
        return {};
    if (value.is_numeric())
        return {Cell::Kind::Number, static_cast<long double>(value.to_f64()), {}};
    return {Cell::Kind::Text, 0, value.to_val_string()};
}

std::optional<std::string> compare_results(
    const QuerySample & popodb, const QuerySample & sqlite, long double absolute = 1e-6L, long double relative = 1e-8L)
{
    if (popodb.status != "ok" || sqlite.status != "ok")
        return "one or both engines returned an error";
    if (popodb.result.size() != sqlite.result.size())
        return "row count differs: popodb=" + std::to_string(popodb.result.size())
            + " sqlite=" + std::to_string(sqlite.result.size());
    for (size_t row = 0; row < popodb.result.size(); ++row)
    {
        if (popodb.result[row].size() != sqlite.result[row].size())
            return "column count differs at row " + std::to_string(row);
        for (size_t column = 0; column < popodb.result[row].size(); ++column)
        {
            const Cell & left = popodb.result[row][column];
            const Cell & right = sqlite.result[row][column];
            if (left.kind != right.kind)
                return "type differs at row " + std::to_string(row) + " column " + std::to_string(column);
            if (left.kind == Cell::Kind::Text && left.text != right.text)
                return "text differs at row " + std::to_string(row) + " column " + std::to_string(column)
                    + ": popodb=" + left.text + " sqlite=" + right.text;
            if (left.kind == Cell::Kind::Number)
            {
                long double delta = std::fabs(left.number - right.number);
                long double limit = absolute + relative * std::max(std::fabs(left.number), std::fabs(right.number));
                if (delta > limit)
                {
                    std::ostringstream message;
                    message << "number differs at row " << row << " column " << column << ": popodb=" << left.number
                            << " sqlite=" << right.number << " delta=" << delta << " tolerance=" << limit;
                    return message.str();
                }
            }
        }
    }
    return std::nullopt;
}

Result<Options> parse_options(int argc, char ** argv)
{
    Options options;
    if (const char * home = std::getenv("TPCH_HOME"); home != nullptr)
        options.generator.tpch_home = home;
    auto value = [&](int & i) -> Result<std::string>
    {
        if (++i >= argc)
            return popo::base::make_error(Errno::InvalidArg, "missing value for " + std::string(argv[i - 1]));
        return std::string(argv[i]);
    };
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--resume")
            options.resume = true;
        else if (arg == "--force")
            options.force = true;
        else if (arg == "--tpch-home")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            options.generator.tpch_home = *v;
        }
        else if (arg == "--dbgen")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            options.generator.dbgen = *v;
        }
        else if (arg == "--qgen")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            options.generator.qgen = *v;
        }
        else if (arg == "--scale")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            auto parsed = popo::base::parse_f64(*v);
            if (!parsed)
                return std::unexpected(parsed.error());
            options.generator.scale_factor = *parsed;
        }
        else if (arg == "--data-dir")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            options.generator.data_dir = *v;
        }
        else if (arg == "--db-dir")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            options.database_dir = *v;
        }
        else if (arg == "--result-dir")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            options.result_dir = *v;
        }
        else if (arg == "--engine")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            options.engine = *v;
        }
        else if (arg == "--phase")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            options.phase = *v;
        }
        else if (arg == "--batch-rows")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            auto parsed = popo::base::parse_i64(*v);
            if (!parsed || *parsed < 1)
                return popo::base::make_error(Errno::InvalidArg, "batch rows must be positive");
            options.batch_rows = static_cast<size_t>(*parsed);
        }
        else if (arg == "--skip-queries")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            size_t start = 0;
            while (start <= v->size())
            {
                const auto comma = v->find(',', start);
                const auto piece = v->substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!piece.empty())
                {
                    auto parsed = popo::base::parse_i64(piece);
                    if (!parsed || *parsed < 1 || *parsed > 22)
                        return popo::base::make_error(Errno::InvalidArg, "skip queries must be in 1..22");
                    options.skip[static_cast<size_t>(*parsed)] = true;
                }
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
        }
        else if (arg == "--memory-budget")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            auto parsed = popo::base::parse_i64(*v);
            if (!parsed || *parsed < 4096)
                return popo::base::make_error(Errno::InvalidArg, "memory budget must be at least 4096 bytes");
            options.executor_memory_bytes = static_cast<size_t>(*parsed);
        }
        else if (arg == "--buffer-pool-pages")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            auto parsed = popo::base::parse_i64(*v);
            if (!parsed || *parsed < 1)
                return popo::base::make_error(Errno::InvalidArg, "buffer pool pages must be positive");
            options.buffer_pool_pages = static_cast<size_t>(*parsed);
        }
        else if (arg == "--temp-dir")
        {
            auto v = value(i);
            if (!v)
                return std::unexpected(v.error());
            options.temp_dir = *v;
        }
        else if (arg == "--help")
        {
            std::cout << "tpch_bench --tpch-home PATH [--dbgen PATH] [--qgen PATH] --scale N\n"
                      << "  --data-dir PATH --db-dir PATH --result-dir PATH\n"
                      << "  [--engine popodb|sqlite|both] [--phase generate|load|query|all]\n"
                      << "  [--batch-rows N] [--memory-budget BYTES] [--buffer-pool-pages N]\n"
              << "  [--skip-queries 4,17,20,21,22]\n"
                      << "  [--temp-dir PATH] [--resume] [--force]\n";
            std::exit(0);
        }
        else
            return popo::base::make_error(Errno::InvalidArg, "unknown option: " + arg);
    }
    if (options.generator.data_dir.empty() || options.database_dir.empty() || options.result_dir.empty())
        return popo::base::make_error(Errno::InvalidArg, "--data-dir, --db-dir, and --result-dir are required");
    if (options.generator.tpch_home.empty() && (options.generator.dbgen.empty() || options.generator.qgen.empty()))
        return popo::base::make_error(Errno::InvalidArg, "provide --tpch-home or both --dbgen and --qgen");
    if (options.engine != "popodb" && options.engine != "sqlite" && options.engine != "both")
        return popo::base::make_error(Errno::InvalidArg, "engine must be popodb, sqlite, or both");
    if (options.phase != "generate" && options.phase != "load" && options.phase != "query" && options.phase != "all")
        return popo::base::make_error(Errno::InvalidArg, "phase must be generate, load, query, or all");
    return options;
}

Result<void> ensure_directory(const std::filesystem::path & path)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec)
        return popo::base::make_error(Errno::IoErr, "create " + path.string() + ": " + ec.message());
    return {};
}

Result<uint64_t> tree_bytes(const std::filesystem::path & root)
{
    uint64_t total = 0;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        return total;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
    {
        if (ec)
            return popo::base::make_error(Errno::IoErr, "scan " + root.string() + ": " + ec.message());
        if (it->is_regular_file(ec))
            total += it->file_size(ec);
        if (ec)
            return popo::base::make_error(Errno::IoErr, "inspect " + it->path().string() + ": " + ec.message());
    }
    return total;
}

Result<uint64_t> path_bytes(const std::filesystem::path & path)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec))
    {
        uint64_t bytes = std::filesystem::file_size(path, ec);
        if (!ec)
            return bytes;
    }
    if (ec)
        return popo::base::make_error(Errno::IoErr, "inspect " + path.string() + ": " + ec.message());
    return tree_bytes(path);
}

std::string first_line(const std::filesystem::path & path)
{
    std::ifstream input(path);
    std::string line;
    if (input)
        std::getline(input, line);
    return line;
}

std::string git_revision(std::filesystem::path root)
{
    auto git = root / ".git";
    std::string head = first_line(git / "HEAD");
    constexpr std::string_view prefix = "ref: ";
    if (head.starts_with(prefix))
        return first_line(git / head.substr(prefix.size()));
    return head;
}

std::string cpu_model()
{
    std::ifstream input("/proc/cpuinfo");
    std::string line;
    while (std::getline(input, line))
    {
        if (line.starts_with("model name"))
        {
            size_t separator = line.find(':');
            if (separator != std::string::npos)
                return std::string(popo::base::trim_ascii(std::string_view(line).substr(separator + 1)));
        }
    }
    return {};
}

Result<void> validate_data(const std::filesystem::path & dir)
{
    for (const auto & table : popo::tpch::tables())
    {
        if (!std::filesystem::is_regular_file(dir / table.file_name))
            return popo::base::make_error(Errno::InvalidArg, "missing data file: " + (dir / table.file_name).string());
    }
    return {};
}

Result<std::vector<std::string>> split_sql(std::string_view sql)
{
    std::vector<std::string> statements;
    bool quoted = false;
    size_t start = 0;
    for (size_t i = 0; i < sql.size(); ++i)
    {
        if (sql[i] == '\'' && (i + 1 >= sql.size() || sql[i + 1] != '\''))
            quoted = !quoted;
        else if (sql[i] == '\'' && quoted && i + 1 < sql.size() && sql[i + 1] == '\'')
            ++i;
        else if (sql[i] == ';' && !quoted)
        {
            if (i > start)
                statements.emplace_back(sql.substr(start, i - start));
            start = i + 1;
        }
    }
    if (quoted)
        return popo::base::make_error(Errno::InvalidArg, "unterminated SQL string");
    if (start < sql.size())
        statements.emplace_back(sql.substr(start));
    return statements;
}

Result<void> load_popodb(const Options & options, const std::filesystem::path & directory)
{
    popo::test::SqlRunner db(directory.string(), options.buffer_pool_pages);
    for (const auto & statement : popo::tpch::schema_statements(popo::tpch::Dialect::PopoDB))
    {
        auto result = db.exec(statement);
        if (!result)
            return std::unexpected(result.error());
    }
    for (const auto & table : popo::tpch::tables())
    {
        std::ifstream input(options.generator.data_dir / table.file_name);
        if (!input)
            return popo::base::make_error(
                Errno::IoErr, "open " + (options.generator.data_dir / table.file_name).string());
        std::optional<popo::test::SqlTxn> transaction;
        size_t in_batch = 0;
        std::string line;
        while (std::getline(input, line))
        {
            if (!transaction)
            {
                auto begun = db.begin();
                if (!begun)
                    return std::unexpected(begun.error());
                transaction.emplace(std::move(*begun));
            }
            auto fields = popo::tpch::read_tbl_row(line, table.column_count);
            if (!fields)
                return std::unexpected(fields.error());
            auto inserted = transaction->exec(popo::tpch::insert_sql(table.name, *fields));
            if (!inserted)
                return std::unexpected(inserted.error());
            if (++in_batch == options.batch_rows)
            {
                TPCH_TRY(transaction->commit());
                transaction.reset();
                in_batch = 0;
            }
        }
        if (input.bad())
            return popo::base::make_error(
                Errno::IoErr, "read " + (options.generator.data_dir / table.file_name).string());
        if (transaction)
            TPCH_TRY(transaction->commit());
    }
    TPCH_TRY(db.engine().flush_all());
    db.engine().checkpoint();
    return {};
}

Result<void> sqlite_exec(sqlite3 * db, std::string_view sql)
{
    char * message = nullptr;
    int rc = sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr, &message);
    if (rc == SQLITE_OK)
        return {};
    std::string error = message != nullptr ? message : sqlite3_errstr(rc);
    sqlite3_free(message);
    return popo::base::make_error(rc, std::move(error));
}

Result<void> load_sqlite(const Options & options, const std::filesystem::path & path)
{
    sqlite3 * raw = nullptr;
    int rc = sqlite3_open(path.c_str(), &raw);
    if (rc != SQLITE_OK)
    {
        std::string error = raw != nullptr ? sqlite3_errmsg(raw) : sqlite3_errstr(rc);
        if (raw != nullptr)
            sqlite3_close(raw);
        return popo::base::make_error(rc, std::move(error));
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    TPCH_TRY(sqlite_exec(db.get(), "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA cache_size=-2097152"));
    for (const auto & statement : popo::tpch::schema_statements(popo::tpch::Dialect::SQLite))
        TPCH_TRY(sqlite_exec(db.get(), statement));
    for (const auto & table : popo::tpch::tables())
    {
        std::ifstream input(options.generator.data_dir / table.file_name);
        if (!input)
            return popo::base::make_error(
                Errno::IoErr, "open " + (options.generator.data_dir / table.file_name).string());
        std::string sql = "INSERT INTO " + std::string(table.name) + " VALUES (?";
        for (size_t i = 1; i < table.column_count; ++i)
            sql += ",?";
        sql += ")";
        sqlite3_stmt * raw_statement = nullptr;
        rc = sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &raw_statement, nullptr);
        if (rc != SQLITE_OK)
            return popo::base::make_error(rc, sqlite3_errmsg(db.get()));
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw_statement, sqlite3_finalize);
        TPCH_TRY(sqlite_exec(db.get(), "BEGIN IMMEDIATE"));
        size_t in_batch = 0;
        std::string line;
        while (std::getline(input, line))
        {
            auto fields = popo::tpch::read_tbl_row(line, table.column_count);
            if (!fields)
                return std::unexpected(fields.error());
            sqlite3_reset(statement.get());
            sqlite3_clear_bindings(statement.get());
            for (size_t i = 0; i < fields->size(); ++i)
                sqlite3_bind_text(statement.get(), static_cast<int>(i + 1), (*fields)[i].c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(statement.get());
            if (rc != SQLITE_DONE)
                return popo::base::make_error(rc, sqlite3_errmsg(db.get()));
            if (++in_batch == options.batch_rows)
            {
                TPCH_TRY(sqlite_exec(db.get(), "COMMIT"));
                TPCH_TRY(sqlite_exec(db.get(), "BEGIN IMMEDIATE"));
                in_batch = 0;
            }
        }
        TPCH_TRY(sqlite_exec(db.get(), "COMMIT"));
        if (input.bad())
            return popo::base::make_error(
                Errno::IoErr, "read " + (options.generator.data_dir / table.file_name).string());
    }
    TPCH_TRY(sqlite_exec(db.get(), "PRAGMA wal_checkpoint(TRUNCATE)"));
    return {};
}

Result<QuerySample> run_popodb_query(popo::test::SqlRunner & db, size_t number, std::string_view query)
{
    QuerySample sample;
    sample.query = number;
    auto statements = split_sql(query);
    if (!statements)
        return std::unexpected(statements.error());
    auto start = Clock::now();
    RssMonitor rss;
    bool saw_result = false;
    for (size_t i = 0; i < statements->size(); ++i)
    {
        auto result = db.exec((*statements)[i]);
        if (!result)
        {
            // qgen Q15 creates revenue0 before its SELECT. Always attempt the
            // matching DROP on failure so --resume can retry in this process.
            if (number == 15)
                (void)db.exec("drop view revenue0");
            return std::unexpected(result.error());
        }
        sample.executor_peak_bytes = std::max<uint64_t>(sample.executor_peak_bytes, result->spill.peak_memory_bytes);
        sample.spill_bytes += result->spill.bytes_written;
        sample.spill_files += result->spill.files_created;
        if (!result->result.columns.empty() || !result->result.rows.empty())
        {
            saw_result = true;
            sample.result.clear();
            for (const auto & row : result->result.rows)
            {
                std::vector<Cell> cells;
                cells.reserve(row->values_.size());
                for (const auto & value : row->values_)
                    cells.push_back(popodb_cell(value));
                sample.result.push_back(std::move(cells));
            }
        }
    }
    if (number == 15 && statements->size() < 3)
        (void)db.exec("drop view revenue0");
    if (!saw_result)
        sample.result.clear();
    sample.rows = sample.result.size();
    sample.digest = result_digest(sample.result);
    sample.peak_rss_bytes = rss.finish();
    sample.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return sample;
}

Result<QuerySample> run_sqlite_query(sqlite3 * db, size_t number, std::string_view query)
{
    QuerySample sample;
    sample.query = number;
    auto start = Clock::now();
    RssMonitor rss;
    const char * tail = query.data();
    const char * end = query.data() + query.size();
    while (tail < end)
    {
        sqlite3_stmt * raw = nullptr;
        int rc = sqlite3_prepare_v2(db, tail, static_cast<int>(end - tail), &raw, &tail);
        if (rc != SQLITE_OK)
            return popo::base::make_error(rc, sqlite3_errmsg(db));
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
        if (raw == nullptr)
            continue;
        size_t rows = 0;
        while ((rc = sqlite3_step(raw)) == SQLITE_ROW)
        {
            std::vector<Cell> row;
            int columns = sqlite3_column_count(raw);
            row.reserve(static_cast<size_t>(columns));
            for (int column = 0; column < columns; ++column)
            {
                int type = sqlite3_column_type(raw, column);
                if (type == SQLITE_NULL)
                    row.push_back({});
                else if (type == SQLITE_INTEGER || type == SQLITE_FLOAT)
                    row.push_back({Cell::Kind::Number, sqlite3_column_double(raw, column), {}});
                else
                {
                    const auto * text = reinterpret_cast<const char *>(sqlite3_column_text(raw, column));
                    int bytes = sqlite3_column_bytes(raw, column);
                    row.push_back({Cell::Kind::Text, 0, std::string(text, static_cast<size_t>(bytes))});
                }
            }
            sample.result.push_back(std::move(row));
            ++rows;
        }
        if (rc != SQLITE_DONE)
            return popo::base::make_error(rc, sqlite3_errmsg(db));
        sample.rows = std::max(sample.rows, rows);
    }
    sample.rows = sample.result.size();
    sample.digest = result_digest(sample.result);
    sample.peak_rss_bytes = rss.finish();
    sample.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return sample;
}

Result<void> write_sample(const Options & options, std::string_view engine, const QuerySample & sample)
{
    auto path = options.result_dir
        / (std::string(engine) + "-q" + (sample.query < 10 ? "0" : "") + std::to_string(sample.query) + ".json");
    std::ofstream json(path, std::ios::trunc);
    if (!json)
        return popo::base::make_error(Errno::IoErr, "write " + path.string());
    json << "{\n"
         << "  \"engine\": " << json_quote(engine) << ",\n"
         << "  \"scale_factor\": " << options.generator.scale_factor << ",\n"
         << "  \"query\": " << sample.query << ",\n"
         << "  \"milliseconds\": " << sample.milliseconds << ",\n"
         << "  \"rows\": " << sample.rows << ",\n"
         << "  \"canonical_digest\": " << json_quote(sample.digest) << ",\n"
         << "  \"peak_rss_bytes\": " << sample.peak_rss_bytes << ",\n"
         << "  \"executor_peak_bytes\": " << sample.executor_peak_bytes << ",\n"
         << "  \"spill_files\": " << sample.spill_files << ",\n"
         << "  \"spill_bytes\": " << sample.spill_bytes << ",\n"
         << "  \"status\": " << json_quote(sample.status) << ",\n"
         << "  \"error\": " << json_quote(sample.error) << "\n"
         << "}\n";
    return {};
}

Result<void> run_queries(const Options & options, std::string_view engine, const std::filesystem::path & database)
{
    const auto dialect = engine == "sqlite" ? popo::tpch::Dialect::SQLite : popo::tpch::Dialect::PopoDB;
    auto queries = popo::tpch::qgen_queries(options.generator, dialect);
    if (!queries)
        return std::unexpected(queries.error());
    std::ofstream csv(options.result_dir / (std::string(engine) + "-queries.csv"), std::ios::app);
    if (!csv)
        return popo::base::make_error(Errno::IoErr, "open query CSV");
    if (csv.tellp() == 0)
        csv << "engine,scale_factor,query,milliseconds,rows,digest,peak_rss_bytes,"
               "executor_peak_bytes,spill_files,spill_bytes,status,error\n";

    std::unique_ptr<popo::test::SqlRunner> popodb;
    sqlite3 * sqlite_raw = nullptr;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> sqlite(nullptr, sqlite3_close);
    if (engine == "sqlite")
    {
        int rc = sqlite3_open(database.c_str(), &sqlite_raw);
        if (rc != SQLITE_OK)
            return popo::base::make_error(rc, sqlite_raw != nullptr ? sqlite3_errmsg(sqlite_raw) : sqlite3_errstr(rc));
        sqlite.reset(sqlite_raw);
        TPCH_TRY(sqlite_exec(sqlite.get(), "PRAGMA cache_size=-2097152"));
    }
    else
    {
        popo::executor::Executor::Options executor_options;
        executor_options.memory_limit_bytes = options.executor_memory_bytes;
        executor_options.temp_directory = options.temp_dir;
        popodb
            = std::make_unique<popo::test::SqlRunner>(database.string(), options.buffer_pool_pages, executor_options);
    }

    for (size_t i = 0; i < queries->size(); ++i)
    {
        auto json_path = options.result_dir
            / (std::string(engine) + "-q" + (i + 1 < 10 ? "0" : "") + std::to_string(i + 1) + ".json");
        if (options.resume && std::filesystem::is_regular_file(json_path))
            continue;
        QuerySample sample;
        sample.query = i + 1;
        if (options.skip[i + 1])
        {
            sample.status = "skipped";
        }
        else
        {
            Result<QuerySample> result = engine == "sqlite" ? run_sqlite_query(sqlite.get(), i + 1, (*queries)[i])
                                                            : run_popodb_query(*popodb, i + 1, (*queries)[i]);
            if (result)
                sample = std::move(*result);
            else
            {
                sample.status = "error";
                sample.error = result.error().error_message;
            }
        }
        TPCH_TRY(write_sample(options, engine, sample));
        csv << engine << ',' << options.generator.scale_factor << ',' << sample.query << ',' << sample.milliseconds
            << ',' << sample.rows << ',' << sample.digest << ',' << sample.peak_rss_bytes << ','
            << sample.executor_peak_bytes << ',' << sample.spill_files << ',' << sample.spill_bytes << ','
            << sample.status << ',' << csv_quote(sample.error) << '\n';
        csv.flush();
    }
    return {};
}

Result<void> write_metadata(const Options & options)
{
    TPCH_TRY(ensure_directory(options.result_dir));
    auto data_bytes = tree_bytes(options.generator.data_dir);
    if (!data_bytes)
        return std::unexpected(data_bytes.error());
    std::ofstream out(options.result_dir / "metadata.json", std::ios::trunc);
    if (!out)
        return popo::base::make_error(Errno::IoErr, "write metadata");
    out << "{\n"
        << "  \"scale_factor\": " << options.generator.scale_factor << ",\n"
        << "  \"project_git_commit\": " << json_quote(git_revision(std::filesystem::current_path())) << ",\n"
        << "  \"generator_git_commit\": " << json_quote(git_revision(options.generator.tpch_home)) << ",\n"
        << "  \"generator_version\": " << json_quote("TPC-H dbgen/qgen " + git_revision(options.generator.tpch_home))
        << ",\n"
        << "  \"tpch_home\": " << json_quote(options.generator.tpch_home.string()) << ",\n"
        << "  \"dbgen\": " << json_quote(options.generator.dbgen.string()) << ",\n"
        << "  \"qgen\": " << json_quote(options.generator.qgen.string()) << ",\n"
        << "  \"data_dir\": " << json_quote(options.generator.data_dir.string()) << ",\n"
        << "  \"database_dir\": " << json_quote(options.database_dir.string()) << ",\n"
        << "  \"result_dir\": " << json_quote(options.result_dir.string()) << ",\n"
        << "  \"engine\": " << json_quote(options.engine) << ",\n"
        << "  \"phase\": " << json_quote(options.phase) << ",\n"
        << "  \"data_bytes\": " << *data_bytes << ",\n"
        << "  \"executor_memory_budget_bytes\": " << options.executor_memory_bytes << ",\n"
        << "  \"buffer_pool_pages\": " << options.buffer_pool_pages << ",\n"
        << "  \"numeric_absolute_tolerance\": 1e-6,\n"
        << "  \"numeric_relative_tolerance\": 1e-8,\n"
        << "  \"cpu_model\": " << json_quote(cpu_model()) << ",\n"
        << "  \"hardware_threads\": " << std::thread::hardware_concurrency() << ",\n"
#ifdef NDEBUG
        << "  \"build_mode\": \"release\"\n"
#else
        << "  \"build_mode\": \"debug\"\n"
#endif
        << "}\n";
    return {};
}

Result<void> prepare_database(const Options & options, const std::filesystem::path & path)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
    {
        // A completed load is skipped by its marker before this function is
        // reached. Under --resume, an unmarked database is an interrupted
        // load and is rebuilt from the streaming source.
        if (!options.force && !options.resume)
            return popo::base::make_error(Errno::InvalidArg, path.string() + " exists; use --resume or --force");
        std::filesystem::remove_all(path, ec);
        if (ec)
            return popo::base::make_error(Errno::IoErr, "remove " + path.string() + ": " + ec.message());
    }
    return ensure_directory(path.parent_path());
}

Result<void> run_engine(const Options & options, std::string_view engine)
{
    const bool do_load = options.phase == "load" || options.phase == "all";
    const bool do_query = options.phase == "query" || options.phase == "all";
    const auto database = engine == "sqlite" ? options.database_dir / "sqlite.db" : options.database_dir / "popodb";
    const auto marker = options.result_dir / (std::string(engine) + "-load.done");
    if (do_load && !(options.resume && std::filesystem::is_regular_file(marker)))
    {
        TPCH_TRY(prepare_database(options, database));
        auto started = Clock::now();
        auto loaded = engine == "sqlite" ? load_sqlite(options, database) : load_popodb(options, database);
        if (!loaded)
            return std::unexpected(loaded.error());
        double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        auto database_bytes = path_bytes(database);
        if (!database_bytes)
            return std::unexpected(database_bytes.error());
        std::ofstream load_json(options.result_dir / (std::string(engine) + "-load.json"), std::ios::trunc);
        if (!load_json)
            return popo::base::make_error(Errno::IoErr, "write load JSON");
        load_json << "{\n"
                  << "  \"engine\": " << json_quote(engine) << ",\n"
                  << "  \"scale_factor\": " << options.generator.scale_factor << ",\n"
                  << "  \"milliseconds\": " << milliseconds << ",\n"
                  << "  \"database_bytes\": " << *database_bytes << ",\n"
                  << "  \"status\": \"ok\"\n"
                  << "}\n";
        std::ofstream(marker) << "complete\n";
    }
    if (do_query)
    {
        if (!std::filesystem::exists(database))
            return popo::base::make_error(Errno::InvalidArg, "database does not exist: " + database.string());
        TPCH_TRY(run_queries(options, engine, database));
    }
    return {};
}

Result<void> run(const Options & options)
{
    TPCH_TRY(ensure_directory(options.result_dir));
    if (options.phase == "generate" || options.phase == "all")
    {
        if (!(options.resume && validate_data(options.generator.data_dir)))
            TPCH_TRY(popo::tpch::generate_data(options.generator));
    }
    if (options.phase != "generate")
        TPCH_TRY(validate_data(options.generator.data_dir));
    TPCH_TRY(write_metadata(options));
    if (options.phase == "generate")
        return {};
    if (options.engine == "popodb" || options.engine == "both")
        TPCH_TRY(run_engine(options, "popodb"));
    if (options.engine == "sqlite" || options.engine == "both")
        TPCH_TRY(run_engine(options, "sqlite"));
    if (options.engine == "both" && (options.phase == "query" || options.phase == "all"))
    {
        // Per-query files carry canonical digests and resource evidence. The
        // in-process comparison is performed during a dedicated replay below,
        // avoiding lossy parsing of those JSON summaries.
        auto popo_queries = popo::tpch::qgen_queries(options.generator, popo::tpch::Dialect::PopoDB);
        auto sqlite_queries = popo::tpch::qgen_queries(options.generator, popo::tpch::Dialect::SQLite);
        if (!popo_queries)
            return std::unexpected(popo_queries.error());
        if (!sqlite_queries)
            return std::unexpected(sqlite_queries.error());
        popo::executor::Executor::Options executor_options;
        executor_options.memory_limit_bytes = options.executor_memory_bytes;
        executor_options.temp_directory = options.temp_dir;
        popo::test::SqlRunner popodb(
            (options.database_dir / "popodb").string(), options.buffer_pool_pages, executor_options);
        sqlite3 * raw = nullptr;
        int rc = sqlite3_open((options.database_dir / "sqlite.db").c_str(), &raw);
        if (rc != SQLITE_OK)
            return popo::base::make_error(rc, raw != nullptr ? sqlite3_errmsg(raw) : sqlite3_errstr(rc));
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> sqlite(raw, sqlite3_close);
        std::ofstream comparison(options.result_dir / "comparison.json", std::ios::trunc);
        comparison << "{\n  \"absolute_tolerance\": 1e-6,\n"
                      "  \"relative_tolerance\": 1e-8,\n  \"queries\": [\n";
        bool all_match = true;
        for (size_t i = 0; i < 22; ++i)
        {
            if (options.skip[i + 1])
            {
                comparison << "    {\"query\": " << (i + 1) << ", \"match\": true, \"skipped\": true, \"detail\": "
                           << json_quote("skipped by --skip-queries") << "}" << (i + 1 == 22 ? "\n" : ",\n");
                continue;
            }
            auto left = run_popodb_query(popodb, i + 1, (*popo_queries)[i]);
            auto right = run_sqlite_query(sqlite.get(), i + 1, (*sqlite_queries)[i]);
            std::string mismatch;
            if (!left)
                mismatch = "popodb error: " + left.error().error_message;
            else if (!right)
                mismatch = "sqlite error: " + right.error().error_message;
            else if (auto difference = compare_results(*left, *right))
                mismatch = *difference;
            all_match = all_match && mismatch.empty();
            comparison << "    {\"query\": " << (i + 1) << ", \"match\": " << (mismatch.empty() ? "true" : "false")
                       << ", \"detail\": " << json_quote(mismatch) << "}" << (i + 1 == 22 ? "\n" : ",\n");
        }
        comparison << "  ],\n  \"all_match\": " << (all_match ? "true" : "false") << "\n}\n";
        if (!all_match)
            return popo::base::make_error(Errno::InternalErr, "TPC-H result comparison failed; see comparison.json");
    }
    return {};
}

} // namespace

int main(int argc, char ** argv)
{
    auto options = parse_options(argc, argv);
    if (!options)
    {
        std::cerr << options.error().error_message << '\n';
        return 2;
    }
    auto result = run(*options);
    if (!result)
    {
        std::cerr << result.error().error_message << '\n';
        return 1;
    }
    return 0;
}
