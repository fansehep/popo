#pragma once

#include "src/base/error.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace popo::tpch
{

enum class Dialect
{
    PopoDB,
    SQLite,
};

struct Table
{
    std::string_view name;
    std::string_view file_name;
    size_t column_count;
};

struct GeneratorConfig
{
    std::filesystem::path tpch_home;
    std::filesystem::path dbgen;
    std::filesystem::path qgen;
    std::filesystem::path data_dir;
    double scale_factor = 1.0;
};

const std::array<Table, 8> & tables();
std::vector<std::string> schema_statements(Dialect dialect);

// Stable qgen substitutions used by the focused SQL tests. Benchmark queries
// are always obtained from qgen_queries(), including Q11's scale-dependent
// fraction.
popo::base::Result<std::string> test_query(size_t query_number, Dialect dialect = Dialect::PopoDB);
std::vector<std::string> test_query_setup(size_t query_number, Dialect dialect = Dialect::PopoDB);
std::vector<std::string> test_query_cleanup(size_t query_number, Dialect dialect = Dialect::PopoDB);

popo::base::Result<std::array<std::string, 22>> qgen_queries(const GeneratorConfig & config, Dialect dialect);
popo::base::Result<void> generate_data(const GeneratorConfig & config);

popo::base::Result<std::vector<std::string>> read_tbl_row(std::string_view line, size_t expected_columns);
std::string insert_sql(std::string_view table, std::span<const std::string> fields);
std::string to_dialect(std::string_view sql, Dialect dialect);

} // namespace popo::tpch
