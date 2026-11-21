#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "executor/types/column_schema.h"
#include "parser/ast.h"
#include "type/tuple.h"
#include "type/value.h"

namespace popo::executor
{

struct Batch
{
    std::vector<ColumnSchema> schemas;
    std::vector<popo::type::Row> rows;
    std::optional<std::filesystem::path> spilled_rows;
    // Per-row aggregate results (aligned with rows) when produced by an Agg node,
    // so HAVING / Project / Sort above can reference aggregate expressions.
    std::vector<std::unordered_map<const popo::parser::Expr *, popo::type::Value>> row_aggs;

    size_t width() const;
    std::vector<std::string> flat_names() const;
};

} // namespace popo::executor
