#include "bench/tpch/workload.h"

#include <cerrno>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <regex>
#include <unistd.h>
#include <sys/wait.h>

namespace popo::tpch
{
namespace
{

using popo::base::Errno;
using popo::base::Result;

const std::array<Table, 8> kTables = {{
    {"region", "region.tbl", 3},
    {"nation", "nation.tbl", 4},
    {"part", "part.tbl", 9},
    {"supplier", "supplier.tbl", 7},
    {"partsupp", "partsupp.tbl", 5},
    {"customer", "customer.tbl", 8},
    {"orders", "orders.tbl", 9},
    {"lineitem", "lineitem.tbl", 16},
}};

Result<std::string> run_capture(
    const std::filesystem::path & executable,
    const std::filesystem::path & cwd,
    const std::vector<std::string> & args,
    const std::vector<std::pair<std::string, std::string>> & environment = {})
{
    if (!std::filesystem::is_regular_file(executable))
        return popo::base::make_error(Errno::InvalidArg, "missing executable: " + executable.string());

    int fds[2];
    if (::pipe(fds) != 0)
        return popo::base::make_error(Errno::IoErr, "pipe: " + std::string(std::strerror(errno)));
    pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(fds[0]);
        ::close(fds[1]);
        return popo::base::make_error(Errno::IoErr, "fork: " + std::string(std::strerror(errno)));
    }
    if (pid == 0)
    {
        ::close(fds[0]);
        (void)::dup2(fds[1], STDOUT_FILENO);
        (void)::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        if (::chdir(cwd.c_str()) != 0)
            _exit(126);
        for (const auto & [name, value] : environment)
        {
            if (::setenv(name.c_str(), value.c_str(), 1) != 0)
                _exit(126);
        }
        std::vector<char *> argv;
        std::string exe = executable.string();
        argv.push_back(exe.data());
        for (const auto & arg : args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        ::execv(exe.c_str(), argv.data());
        _exit(127);
    }
    ::close(fds[1]);
    std::string output;
    std::array<char, 8192> buffer{};
    for (;;)
    {
        ssize_t count = ::read(fds[0], buffer.data(), buffer.size());
        if (count > 0)
            output.append(buffer.data(), static_cast<size_t>(count));
        else if (count == 0)
            break;
        else if (errno != EINTR)
        {
            ::close(fds[0]);
            (void)::waitpid(pid, nullptr, 0);
            return popo::base::make_error(Errno::IoErr, "read child output: " + std::string(std::strerror(errno)));
        }
    }
    ::close(fds[0]);
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0)
        return popo::base::make_error(Errno::IoErr, "waitpid: " + std::string(std::strerror(errno)));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return popo::base::make_error(
            Errno::InternalErr, executable.filename().string() + " failed (" + std::to_string(status) + "): " + output);
    return output;
}

std::filesystem::path executable_path(const GeneratorConfig & config, bool queries)
{
    const auto & explicit_path = queries ? config.qgen : config.dbgen;
    if (!explicit_path.empty())
        return std::filesystem::absolute(explicit_path);
    const auto name = queries ? "qgen" : "dbgen";
    auto direct = config.tpch_home / name;
    if (std::filesystem::is_regular_file(direct))
        return std::filesystem::absolute(direct);
    return std::filesystem::absolute(config.tpch_home / "dbgen" / name);
}

std::string scale_arg(double scale)
{
    char buffer[64];
    auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), scale);
    return ec == std::errc{} ? std::string(buffer, end) : std::string();
}

#include "bench/tpch/workload_queries.inc"

} // namespace

const std::array<Table, 8> & tables()
{
    return kTables;
}

std::vector<std::string> schema_statements(Dialect)
{
    // Primary keys are deliberately identical for both engines. TPC-H's
    // foreign keys are informational and no query-specific indexes are added.
    return {
        "CREATE TABLE region (r_regionkey INTEGER NOT NULL, r_name CHAR(25) NOT NULL, "
        "r_comment VARCHAR(152), PRIMARY KEY (r_regionkey))",
        "CREATE TABLE nation (n_nationkey INTEGER NOT NULL, n_name CHAR(25) NOT NULL, "
        "n_regionkey INTEGER NOT NULL, n_comment VARCHAR(152), PRIMARY KEY (n_nationkey), "
        "FOREIGN KEY (n_regionkey) REFERENCES region(r_regionkey))",
        "CREATE TABLE part (p_partkey INTEGER NOT NULL, p_name VARCHAR(55) NOT NULL, "
        "p_mfgr CHAR(25) NOT NULL, p_brand CHAR(10) NOT NULL, p_type VARCHAR(25) NOT NULL, "
        "p_size INTEGER NOT NULL, p_container CHAR(10) NOT NULL, p_retailprice DECIMAL(15,2) NOT NULL, "
        "p_comment VARCHAR(23) NOT NULL, PRIMARY KEY (p_partkey))",
        "CREATE TABLE supplier (s_suppkey INTEGER NOT NULL, s_name CHAR(25) NOT NULL, "
        "s_address VARCHAR(40) NOT NULL, s_nationkey INTEGER NOT NULL, s_phone CHAR(15) NOT NULL, "
        "s_acctbal DECIMAL(15,2) NOT NULL, s_comment VARCHAR(101) NOT NULL, PRIMARY KEY (s_suppkey), "
        "FOREIGN KEY (s_nationkey) REFERENCES nation(n_nationkey))",
        "CREATE TABLE partsupp (ps_partkey INTEGER NOT NULL, ps_suppkey INTEGER NOT NULL, "
        "ps_availqty INTEGER NOT NULL, ps_supplycost DECIMAL(15,2) NOT NULL, ps_comment VARCHAR(199) NOT NULL, "
        "PRIMARY KEY (ps_partkey, ps_suppkey), FOREIGN KEY (ps_partkey) REFERENCES part(p_partkey), "
        "FOREIGN KEY (ps_suppkey) REFERENCES supplier(s_suppkey))",
        "CREATE TABLE customer (c_custkey INTEGER NOT NULL, c_name CHAR(25) NOT NULL, "
        "c_address VARCHAR(40) NOT NULL, c_nationkey INTEGER NOT NULL, c_phone CHAR(15) NOT NULL, "
        "c_acctbal DECIMAL(15,2) NOT NULL, c_mktsegment CHAR(10) NOT NULL, c_comment VARCHAR(117) NOT NULL, "
        "PRIMARY KEY (c_custkey), FOREIGN KEY (c_nationkey) REFERENCES nation(n_nationkey))",
        "CREATE TABLE orders (o_orderkey INTEGER NOT NULL, o_custkey INTEGER NOT NULL, "
        "o_orderstatus CHAR(1) NOT NULL, o_totalprice DECIMAL(15,2) NOT NULL, o_orderdate DATE NOT NULL, "
        "o_orderpriority CHAR(15) NOT NULL, o_clerk CHAR(15) NOT NULL, o_shippriority INTEGER NOT NULL, "
        "o_comment VARCHAR(79) NOT NULL, PRIMARY KEY (o_orderkey), "
        "FOREIGN KEY (o_custkey) REFERENCES customer(c_custkey))",
        "CREATE TABLE lineitem (l_orderkey INTEGER NOT NULL, l_partkey INTEGER NOT NULL, "
        "l_suppkey INTEGER NOT NULL, l_linenumber INTEGER NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, "
        "l_extendedprice DECIMAL(15,2) NOT NULL, l_discount DECIMAL(15,2) NOT NULL, "
        "l_tax DECIMAL(15,2) NOT NULL, l_returnflag CHAR(1) NOT NULL, l_linestatus CHAR(1) NOT NULL, "
        "l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, l_receiptdate DATE NOT NULL, "
        "l_shipinstruct CHAR(25) NOT NULL, l_shipmode CHAR(10) NOT NULL, l_comment VARCHAR(44) NOT NULL, "
        "PRIMARY KEY (l_orderkey, l_linenumber), FOREIGN KEY (l_orderkey) REFERENCES orders(o_orderkey), "
        "FOREIGN KEY (l_partkey, l_suppkey) REFERENCES partsupp(ps_partkey, ps_suppkey))",
    };
}

Result<std::string> test_query(size_t query_number, Dialect dialect)
{
    if (query_number < 1 || query_number > kTestQueries.size())
        return popo::base::make_error(Errno::InvalidArg, "TPC-H query number must be in [1, 22]");
    return to_dialect(kTestQueries[query_number - 1], dialect);
}

std::vector<std::string> test_query_setup(size_t query_number, Dialect dialect)
{
    if (query_number != 15)
        return {};
    return {to_dialect(
        "create view revenue0 (supplier_no, total_revenue) as "
        "select l_suppkey, sum(l_extendedprice * (1 - l_discount)) "
        "from lineitem where l_shipdate >= date '1996-01-01' "
        "and l_shipdate < date '1996-01-01' + interval '3' month "
        "group by l_suppkey",
        dialect)};
}

std::vector<std::string> test_query_cleanup(size_t query_number, Dialect dialect)
{
    if (query_number != 15)
        return {};
    return {to_dialect("drop view revenue0", dialect)};
}

Result<std::array<std::string, 22>> qgen_queries(const GeneratorConfig & config, Dialect dialect)
{
    if (!(config.scale_factor > 0))
        return popo::base::make_error(Errno::InvalidArg, "scale factor must be positive");
    const auto qgen = executable_path(config, true);
    const auto cwd = qgen.parent_path();
    std::array<std::string, 22> queries;
    for (size_t i = 0; i < queries.size(); ++i)
    {
        auto generated = run_capture(
            qgen,
            cwd,
            {"-d", "-N", "-s", scale_arg(config.scale_factor), std::to_string(i + 1)},
            {{"DSS_QUERY", (cwd / "queries").string()}});
        if (!generated)
            return std::unexpected(generated.error());
        queries[i] = to_dialect(*generated, dialect);
    }
    return queries;
}

Result<void> generate_data(const GeneratorConfig & config)
{
    if (!(config.scale_factor > 0) || config.data_dir.empty())
        return popo::base::make_error(Errno::InvalidArg, "positive scale and data directory are required");
    std::error_code ec;
    std::filesystem::create_directories(config.data_dir, ec);
    if (ec)
        return popo::base::make_error(Errno::IoErr, "create data directory: " + ec.message());
    const auto dbgen = executable_path(config, false);
    auto generated = run_capture(
        dbgen,
        dbgen.parent_path(),
        {"-f", "-s", scale_arg(config.scale_factor)},
        {{"DSS_PATH", std::filesystem::absolute(config.data_dir).string()}});
    if (!generated)
        return std::unexpected(generated.error());
    return {};
}

Result<std::vector<std::string>> read_tbl_row(std::string_view line, size_t expected_columns)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (start < line.size())
    {
        size_t separator = line.find('|', start);
        if (separator == std::string_view::npos)
            return popo::base::make_error(Errno::InvalidArg, "TPC-H row is missing trailing '|'");
        fields.emplace_back(line.substr(start, separator - start));
        start = separator + 1;
    }
    if (fields.size() != expected_columns)
        return popo::base::make_error(
            Errno::InvalidArg, std::format("TPC-H row has {} columns, expected {}", fields.size(), expected_columns));
    return fields;
}

std::string insert_sql(std::string_view table, std::span<const std::string> fields)
{
    std::string sql = "INSERT INTO ";
    sql += table;
    sql += " VALUES (";
    for (size_t i = 0; i < fields.size(); ++i)
    {
        if (i != 0)
            sql.push_back(',');
        sql.push_back('\'');
        for (char c : fields[i])
        {
            sql.push_back(c);
            if (c == '\'')
                sql.push_back('\'');
        }
        sql.push_back('\'');
    }
    sql.push_back(')');
    return sql;
}

std::string trim(const std::string & text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::vector<std::string> split_top_level(const std::string & text, char separator)
{
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '(')
            ++depth;
        else if (text[i] == ')')
            --depth;
        else if (text[i] == separator && depth == 0)
        {
            parts.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    parts.push_back(trim(text.substr(start)));
    return parts;
}

// SQLite has no derived-table column alias list, so the aliases move onto the
// inner select list: "(select a, count(b) ...) as t(x,y)" becomes
// "(select a as x, count(b) as y ...) as t".
std::string inline_derived_aliases(const std::string & sql)
{
    const std::regex alias_list(R"RX(\)\s*as\s+([a-zA-Z_][a-zA-Z_0-9]*)\s*\(([^()]*)\))RX", std::regex::icase);
    std::string out = sql;
    std::smatch m;
    while (std::regex_search(out, m, alias_list))
    {
        const size_t close = static_cast<size_t>(m.position(0));
        size_t open = close;
        int depth = 0;
        bool matched = false;
        while (open > 0)
        {
            --open;
            if (out[open] == ')')
                ++depth;
            else if (out[open] == '(')
            {
                if (depth == 0)
                {
                    matched = true;
                    break;
                }
                --depth;
            }
        }
        if (!matched)
            break;

        const std::string inner = out.substr(open + 1, close - open - 1);
        std::string lower = inner;
        for (char & c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const size_t select_at = lower.find("select");
        if (select_at == std::string::npos)
            break;
        const size_t list_begin = select_at + 6;
        size_t list_end = lower.size();
        int paren_depth = 0;
        for (size_t i = list_begin; i < lower.size(); ++i)
        {
            if (lower[i] == '(')
                ++paren_depth;
            else if (lower[i] == ')')
                --paren_depth;
            else if (paren_depth == 0 && lower.compare(i, 4, "from") == 0 && (i == 0 || !std::isalnum(static_cast<unsigned char>(lower[i - 1]))))
            {
                list_end = i;
                break;
            }
        }

        const auto items = split_top_level(inner.substr(list_begin, list_end - list_begin), ',');
        const auto aliases = split_top_level(m[2].str(), ',');
        if (items.size() != aliases.size())
            break;
        std::string rewritten;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i != 0)
                rewritten += ", ";
            rewritten += items[i] + " as " + aliases[i];
        }
        out = out.substr(0, open + 1) + inner.substr(0, list_begin) + " " + rewritten + " " + inner.substr(list_end) + ") as "
            + m[1].str() + out.substr(close + static_cast<size_t>(m.length(0)));
    }
    return out;
}

std::string to_dialect(std::string_view input, Dialect dialect)
{
    std::string sql(input);
    // qgen emits the template's :n directive as a separate Oracle-style
    // "where rownum <= N;" preamble. TPC-H means it as top-N, so a positive N
    // becomes a trailing LIMIT; -1 is the unlimited form and just disappears.
    const std::regex rownum(R"(where\s+rownum\s*<=\s*(-?[0-9]+)\s*;)", std::regex::icase);
    std::smatch row_limit;
    if (std::regex_search(sql, row_limit, rownum) && row_limit[1].str() != "-1")
    {
        const std::string limit = "\nlimit " + row_limit[1].str();
        sql = std::regex_replace(sql, rownum, "");
        const auto last_semicolon = sql.rfind(';');
        sql.insert(last_semicolon == std::string::npos ? sql.size() : last_semicolon, limit);
    }
    else
    {
        sql = std::regex_replace(sql, rownum, "");
    }
    if (dialect == Dialect::PopoDB)
        return sql;
    sql = std::regex_replace(
        sql,
        std::regex(
            R"(date\s*'([^']+)'\s*([+-])\s*interval\s*'([0-9]+)'\s*(day|month|year)(?:\s*\([0-9]+\))?)",
            std::regex::icase),
        "date('$1','$2$3 $4')");
    // Any remaining typed literal is a plain ISO date; SQLite stores it as text.
    sql = std::regex_replace(sql, std::regex(R"(\bdate\s*'([^']+)')", std::regex::icase), "'$1'");
    sql = std::regex_replace(
        sql,
        std::regex(R"(extract\s*\(\s*year\s+from\s+([^)]+)\))", std::regex::icase),
        "CAST(strftime('%Y',$1) AS INTEGER)");
    sql = std::regex_replace(
        sql,
        std::regex(R"(substring\s*\(\s*([a-zA-Z0-9_.]+)\s+from\s+([0-9]+)\s+for\s+([0-9]+)\s*\))", std::regex::icase),
        "substr($1,$2,$3)");
    return inline_derived_aliases(sql);
}

} // namespace popo::tpch
