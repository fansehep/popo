#pragma once

#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "src/bytecode/vm.h"
#include "src/catalog/schema_mgr.h"
#include "src/storage/row_serializer.h"
#include "src/storage/storage_engine.h"
#include "src/type/tuple.h"

namespace neon::bytecode
{

// Primary-key point select: SELECT col FROM table WHERE pk = int
// Anything else returns nullopt and the caller uses the full parser.
inline std::optional<ExecuteSuccess> try_point_select(
    neon::catalog::SchemaManager & schema_mgr, neon::storage::StorageEngine & engine, std::string_view sql)
{
    auto skip = [&](size_t & i) {
        while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i])))
        {
            ++i;
        }
    };
    auto keyword = [&](size_t & i, std::string_view word) {
        skip(i);
        if (i + word.size() > sql.size())
        {
            return false;
        }
        for (size_t n = 0; n < word.size(); ++n)
        {
            char c = static_cast<char>(std::tolower(static_cast<unsigned char>(sql[i + n])));
            if (c != word[n])
            {
                return false;
            }
        }
        size_t end = i + word.size();
        if (end < sql.size())
        {
            unsigned char next = static_cast<unsigned char>(sql[end]);
            if (std::isalnum(next) || next == '_')
            {
                return false;
            }
        }
        i = end;
        return true;
    };
    auto identifier = [&](size_t & i, std::string & out) {
        skip(i);
        if (i >= sql.size())
        {
            return false;
        }
        unsigned char first = static_cast<unsigned char>(sql[i]);
        if (!(std::isalpha(first) || first == '_'))
        {
            return false;
        }
        size_t begin = i;
        ++i;
        while (i < sql.size())
        {
            unsigned char c = static_cast<unsigned char>(sql[i]);
            if (!(std::isalnum(c) || c == '_'))
            {
                break;
            }
            ++i;
        }
        out.assign(sql.data() + begin, i - begin);
        return true;
    };

    size_t i = 0;
    if (!keyword(i, "select"))
    {
        return std::nullopt;
    }
    std::string column;
    std::string table;
    std::string pk_column;
    if (!identifier(i, column) || !keyword(i, "from") || !identifier(i, table) || !keyword(i, "where") || !identifier(i, pk_column))
    {
        return std::nullopt;
    }
    skip(i);
    if (i >= sql.size() || sql[i] != '=')
    {
        return std::nullopt;
    }
    ++i;
    skip(i);
    if (i >= sql.size())
    {
        return std::nullopt;
    }
    int sign = 1;
    if (sql[i] == '-')
    {
        sign = -1;
        ++i;
    }
    if (i >= sql.size() || !std::isdigit(static_cast<unsigned char>(sql[i])))
    {
        return std::nullopt;
    }
    int64_t key = 0;
    while (i < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i])))
    {
        int digit = sql[i] - '0';
        if (key > (INT64_MAX - digit) / 10)
        {
            return std::nullopt;
        }
        key = key * 10 + digit;
        ++i;
    }
    key *= sign;
    skip(i);
    if (i < sql.size() && sql[i] == ';')
    {
        ++i;
        skip(i);
    }
    if (i != sql.size())
    {
        return std::nullopt;
    }

    auto schema_result = schema_mgr.query_table(table);
    if (!schema_result)
    {
        return std::nullopt;
    }
    const neon::catalog::Schema & schema = *schema_result.value();
    int32_t pk = schema.primary_key_col_idx_;
    if (pk < 0 || static_cast<size_t>(pk) >= schema.columns_.size() || schema.columns_[static_cast<size_t>(pk)].column_name_ != pk_column)
    {
        return std::nullopt;
    }
    int32_t col = -1;
    for (size_t c = 0; c < schema.columns_.size(); ++c)
    {
        if (schema.columns_[c].column_name_ == column)
        {
            col = static_cast<int32_t>(c);
            break;
        }
    }
    if (col < 0)
    {
        return std::nullopt;
    }
    neon::storage::TableHeap * heap = engine.get_table_heap(table);
    if (heap == nullptr)
    {
        return std::nullopt;
    }

    ExecuteSuccess result;
    result.columns.push_back(column);
    auto rid = heap->lookup_pk(neon::type::Value(neon::type::ValueType::Int64, key));
    if (!rid.has_value())
    {
        return result;
    }
    auto tuple = heap->get_row(*rid);
    if (!tuple.has_value())
    {
        return result;
    }
    auto out = std::make_shared<neon::type::Row>();
    out->append_value(neon::storage::RowSerializer::read_column(tuple->data(), tuple->size(), schema, col));
    result.rows.push_back(std::move(out));
    return result;
}

} // namespace neon::bytecode
