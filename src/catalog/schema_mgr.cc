#include "src/catalog/schema_mgr.h"
#include <algorithm>
#include <sstream>
#include <unordered_set>
#include "src/base/error.h"


namespace popo::catalog
{

using popo::base::Errno;
using popo::base::Error;

popo::base::Result<SchemaId> SchemaManager::create_table(const std::string & table_name, const std::vector<Column> & columns)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    // 1. 检查表名是否存在
    const auto it = schema_map_.find(table_name);
    if (it != schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableAlreadyExists, "table already exists: " + table_name));
    }

    // 2. 检查列属性是否合法
    auto col_result = validate_columns(columns);
    if (!col_result)
    {
        return std::unexpected(col_result.error());
    }

    // 3. 创建新的Schema
    auto schema = std::make_shared<Schema>();
    schema->id_ = next_schema_id_++;
    schema->table_name_ = table_name;
    schema->columns_ = columns;
    schema->primary_key_col_idx_ = find_primary_key_column(columns);

    // 4. 添加到映射中
    schema_map_[table_name] = schema;

    return schema->id_;
}

popo::base::Result<void> SchemaManager::delete_table(const std::string & table_name)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    // 检查表是否存在
    const auto it = schema_map_.find(table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    // 删除表
    schema_map_.erase(it);
    return {};
}

popo::base::Result<SchemaRef> SchemaManager::query_table(const std::string & table_name)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    // 检查表是否存在
    const auto it = schema_map_.find(table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    return it->second;
}

popo::base::Result<void> SchemaManager::modify_table(const std::string & table_name, const std::vector<Column> & new_columns)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    // 检查表是否存在
    const auto it = schema_map_.find(table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    // 验证新列定义
    auto col_result = validate_columns(new_columns);
    if (!col_result)
    {
        return std::unexpected(col_result.error());
    }

    // 更新表结构
    it->second->columns_ = new_columns;
    it->second->primary_key_col_idx_ = find_primary_key_column(new_columns);

    return {};
}

popo::base::Result<void> SchemaManager::create_index(
    const std::string & table_name, const std::string & index_name, const std::vector<int32_t> & index_cols, bool is_unique)
{
    std::lock_guard<std::mutex> lgd(this->mtx_);

    // 检查表是否存在
    const auto it = schema_map_.find(table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    // 检查索引是否已存在
    for (const auto & index : it->second->indexes_)
    {
        if (index.index_name_ == index_name)
        {
            return std::unexpected(Error(Errno::InvalidArg, "index already exists"));
        }
    }

    // 验证索引列是否有效
    for (int32_t col_idx : index_cols)
    {
        if (col_idx < 0 || col_idx >= static_cast<int32_t>(it->second->columns_.size()))
        {
            return std::unexpected(Error(Errno::InvalidArg, "invalid column index"));
        }
    }

    // 创建新索引
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

    // 检查表是否存在
    const auto it = schema_map_.find(table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    // 查找并删除索引
    auto & indexes = it->second->indexes_;
    auto index_it = std::find_if(
        indexes.begin(), indexes.end(), [&index_name](const SchemaIndex & index) { return index.index_name_ == index_name; });

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

    // 检查表是否存在
    const auto it = schema_map_.find(table_name);
    if (it == schema_map_.end())
    {
        return std::unexpected(Error(Errno::TableNotFound, "table not found"));
    }

    return it->second->indexes_;
}

popo::base::Result<void> SchemaManager::validate_columns(const std::vector<Column> & columns)
{
    // 检查是否有重复的列名
    std::unordered_set<std::string> column_names;
    int primary_key_count = 0;

    for (const auto & column : columns)
    {
        // 检查列名是否重复
        if (column_names.find(column.column_name_) != column_names.end())
        {
            return std::unexpected(Error(Errno::InvalidColumnDefinition, "duplicate column name"));
        }
        column_names.insert(column.column_name_);

        // 统计主键数量
        if (column.is_primary_key_)
        {
            primary_key_count++;
        }

        // 检查主键约束
        if (column.is_primary_key_ && column.is_nullable_)
        {
            return std::unexpected(Error(Errno::InvalidColumnDefinition, "primary key column cannot be nullable"));
        }
    }

    // 检查是否恰好有一个主键
    if (primary_key_count != 1)
    {
        return std::unexpected(Error(Errno::InvalidColumnDefinition, "table must have exactly one primary key"));
    }

    return {};
}

int32_t SchemaManager::find_primary_key_column(const std::vector<Column> & columns)
{
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].is_primary_key_)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1; // 没有找到主键
}

} // namespace popo::catalog