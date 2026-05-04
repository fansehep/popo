#pragma once

#include <string>
#include <vector>
#include <memory>
#include "src/catalog/column.h"


namespace neon::catalog
{

struct SchemaIndex {
    // 索引名称
    std::string index_name_;
    // 索引列
    std::vector<int32_t> index_cols_;
    // 是否唯一
    bool is_unique_;
};


using SchemaId = int32_t;

struct Schema
{ 
    // 表的Id
    SchemaId id_;
    // 列的数量
    std::vector<Column> columns_;
    // 表的名称
    std::string table_name_;
    // 主键列
    int32_t primary_key_col_idx_;
    // 该表的所有索引
    std::vector<SchemaIndex> indexes_;
};


using SchemaRef = std::shared_ptr<Schema>;

}
