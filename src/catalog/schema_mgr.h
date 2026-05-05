#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "src/base/error.h"
#include "src/catalog/schema.h"

namespace neon::catalog
{

using neon::base::Error;

class SchemaManager
{
public:
    /**
     @param table_name: 需要创建的表名
     @param columns: 表的列信息
     @return: 成功时返回创建表的ID，失败时返回错误信息
    */
    neon::base::Result<SchemaId> create_table(const std::string & table_name, const std::vector<Column> & columns);

    /**
     @param table_name: 需要删除的表名
     @return: 成功时返回true，失败时返回错误信息
    */
    neon::base::Result<void> delete_table(const std::string & table_name);

    /**
     @param table_name: 需要查询的表名
     @return: 成功时返回表的Schema，失败时返回错误信息
    */
    neon::base::Result<SchemaRef> query_table(const std::string & table_name);

    /**
     @param table_name: 需要修改的表名
     @param new_columns: 新的列信息
     @return: 成功时返回true，失败时返回错误信息
    */
    neon::base::Result<void> modify_table(const std::string & table_name, const std::vector<Column> & new_columns);

    /**
     @param table_name: 表名
     @param index_name: 索引名称
     @param index_cols: 索引列
     @param is_unique: 是否唯一索引
     @return: 成功时返回true，失败时返回错误信息
    */
    neon::base::Result<void>
    create_index(const std::string & table_name, const std::string & index_name, const std::vector<int32_t> & index_cols, bool is_unique);

    /**
     @param table_name: 表名
     @param index_name: 索引名称
     @return: 成功时返回true，失败时返回错误信息
    */
    neon::base::Result<void> delete_index(const std::string & table_name, const std::string & index_name);

    /**
     @param table_name: 表名
     @return: 成功时返回所有索引信息，失败时返回错误信息
    */
    neon::base::Result<std::vector<SchemaIndex>> list_indexes(const std::string & table_name);

    /**
     @return: 所有已注册的表名
    */
    std::vector<std::string> all_table_names() const
    {
        std::vector<std::string> names;
        for (const auto & [name, _] : schema_map_)
        {
            names.push_back(name);
        }
        return names;
    }

private:
    std::mutex mtx_;
    std::unordered_map<std::string, neon::catalog::SchemaRef> schema_map_;
    SchemaId next_schema_id_ = 1; // 用于生成唯一的SchemaId

    // 辅助函数：验证列定义
    neon::base::Result<void> validate_columns(const std::vector<Column> & columns);

    // 辅助函数：查找主键列
    int32_t find_primary_key_column(const std::vector<Column> & columns);
};

} // namespace neon::catalog