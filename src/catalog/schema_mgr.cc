#include "src/catalog/schema_mgr.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include "src/base/error.h"


namespace popo::catalog
{

using popo::base::Errno;
using popo::base::Error;

namespace
{

std::string lower_name(const std::string & text)
{
    std::string out = text;
    for (char & c : out)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

template <typename Map>
auto find_ci(Map & map, const std::string & name) -> decltype(map.find(name))
{
    auto exact = map.find(name);
    if (exact != map.end())
        return exact;
    std::string target = lower_name(name);
    for (auto it = map.begin(); it != map.end(); ++it)
    {
        if (lower_name(it->first) == target)
            return it;
    }
    return map.end();
}

} // namespace

popo::base::Result<SchemaId> SchemaManager::create_table(const std::string & table_name, const std::vector<Column> & columns)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    const auto it = find_ci(schema_map_, table_name);
    if (it != schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableAlreadyExists, "table already exists: " + table_name));
    }

    auto col_result = validate_columns(columns);
    if (!col_result)
    {
        return std::unexpected(col_result.error());
    }

    auto schema = std::make_shared<Schema>();
    schema->id_ = next_schema_id_++;
    schema->table_name_ = table_name;
    schema->columns_ = columns;
    schema->primary_key_col_idx_ = find_primary_key_column(columns);

    schema_map_[table_name] = schema;

    return schema->id_;
}

popo::base::Result<void> SchemaManager::delete_table(const std::string & table_name)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    const auto it = find_ci(schema_map_, table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    schema_map_.erase(it);
    return {};
}

popo::base::Result<SchemaRef> SchemaManager::query_table(const std::string & table_name)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    const auto it = find_ci(schema_map_, table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    return it->second;
}

popo::base::Result<void> SchemaManager::modify_table(const std::string & table_name, const std::vector<Column> & new_columns)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    const auto it = find_ci(schema_map_, table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    auto col_result = validate_columns(new_columns);
    if (!col_result)
    {
        return std::unexpected(col_result.error());
    }

    it->second->columns_ = new_columns;
    it->second->primary_key_col_idx_ = find_primary_key_column(new_columns);

    return {};
}

popo::base::Result<void> SchemaManager::create_index(
    const std::string & table_name, const std::string & index_name, const std::vector<int32_t> & index_cols, bool is_unique)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    const auto it = find_ci(schema_map_, table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    for (const auto & index : it->second->indexes_)
    {
        if (lower_name(index.index_name_) == lower_name(index_name))
        {
            return std::unexpected(Error(Errno::InvalidArg, "index already exists"));
        }
    }

    for (int32_t col_idx : index_cols)
    {
        if (col_idx < 0 || col_idx >= static_cast<int32_t>(it->second->columns_.size()))
        {
            return std::unexpected(Error(Errno::InvalidArg, "invalid column index"));
        }
    }

    SchemaIndex new_index;
    new_index.index_name_ = index_name;
    new_index.index_cols_ = index_cols;
    new_index.is_unique_ = is_unique;

    it->second->indexes_.push_back(new_index);

    return {};
}

popo::base::Result<void> SchemaManager::delete_index(const std::string & table_name, const std::string & index_name)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    const auto it = find_ci(schema_map_, table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    auto & indexes = it->second->indexes_;
    auto index_it = std::find_if(
        indexes.begin(),
        indexes.end(),
        [&index_name](const SchemaIndex & index) { return lower_name(index.index_name_) == lower_name(index_name); });

    if (index_it == indexes.end())
    {
        return std::unexpected(Error(Errno::InvalidArg, "index not found"));
    }

    indexes.erase(index_it);

    return {};
}

popo::base::Result<std::vector<SchemaIndex>> SchemaManager::list_indexes(const std::string & table_name)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    const auto it = find_ci(schema_map_, table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    return it->second->indexes_;
}

popo::base::Result<void> SchemaManager::validate_columns(const std::vector<Column> & columns)
{
    if (columns.empty())
    {
        return std::unexpected(Error(Errno::InvalidColumnDefinition, "table must have at least one column"));
    }
    std::unordered_set<std::string> column_names;

    for (const auto & column : columns)
    {
        std::string cname = lower_name(column.column_name_);
        if (column_names.find(cname) != column_names.end())
        {
            return std::unexpected(Error(Errno::InvalidColumnDefinition, "duplicate column name"));
        }
        column_names.insert(cname);

        if (column.is_primary_key_ && column.is_nullable_)
        {
            return std::unexpected(Error(Errno::InvalidColumnDefinition, "primary key column cannot be nullable"));
        }
    }

    return {};
}

int32_t SchemaManager::find_primary_key_column(const std::vector<Column> & columns)
{
    int32_t found = -1;
    int count = 0;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].is_primary_key_)
        {
            found = static_cast<int32_t>(i);
            ++count;
        }
    }
    return count == 1 ? found : -1;
}

popo::base::Result<void> SchemaManager::create_view(
    const std::string & name, popo::parser::StmtPtr query, std::vector<std::string> column_names, bool or_replace)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);
    if (find_ci(schema_map_, name) != schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableAlreadyExists, "name already exists: " + name));
    }
    auto vit = find_ci(views_, name);
    if (vit != views_.end())
    {
        if (!or_replace)
        {
            return std::unexpected(Error(Errno::TableAlreadyExists, "name already exists: " + name));
        }
        view_columns_.erase(vit->first);
        views_.erase(vit);
    }
    views_[name] = std::move(query);
    if (!column_names.empty())
    {
        view_columns_[name] = std::move(column_names);
    }
    return {};
}

popo::base::Result<void> SchemaManager::drop_view(const std::string & name, bool if_exists)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);
    auto vit = find_ci(views_, name);
    if (vit == views_.end())
    {
        if (if_exists)
            return {};
        return std::unexpected(Error(Errno::TableNotFound, "view not found: " + name));
    }
    view_columns_.erase(vit->first);
    views_.erase(vit);
    return {};
}

popo::parser::StmtPtr SchemaManager::find_view(const std::string & name)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);
    auto it = find_ci(views_, name);
    if (it == views_.end())
    {
        return nullptr;
    }
    return it->second;
}

std::vector<std::string> SchemaManager::find_view_columns(const std::string & name)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);
    auto it = find_ci(view_columns_, name);
    if (it == view_columns_.end())
    {
        return {};
    }
    return it->second;
}

} // namespace popo::catalog
