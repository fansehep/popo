#include "src/catalog/schema_mgr.h"
#include "src/catalog/column.h"
#include <gtest/gtest.h>
#include <vector>

namespace neon::catalog {

class SchemaManagerTest : public ::testing::Test {
protected:
    SchemaManager mgr;
    
    void SetUp() override {
        // Setup code if needed
    }
    
    void TearDown() override {
        // Cleanup code if needed
    }
};

TEST_F(SchemaManagerTest, CreateTableTest) {
    // 创建测试列
    std::vector<Column> columns;
    
    // 添加主键列
    Column id_col;
    id_col.column_name_ = "id";
    id_col.is_primary_key_ = true;
    id_col.is_nullable_ = false;
    id_col.data_type_ = neon::type::ValueType::Int32;
    id_col.type_len_ = 4;
    id_col.is_unique_ = true;
    columns.push_back(id_col);
    
    // 添加名字列
    Column name_col;
    name_col.column_name_ = "name";
    name_col.is_primary_key_ = false;
    name_col.is_nullable_ = true;
    name_col.data_type_ = neon::type::ValueType::String;
    name_col.type_len_ = 255;
    name_col.is_unique_ = false;
    columns.push_back(name_col);
    
    // 创建表
    auto result = mgr.create_table("users", columns);
    EXPECT_TRUE(result.first);
    EXPECT_GT(result.second, 0);
}

TEST_F(SchemaManagerTest, QueryTableTest) {
    // 先创建表
    std::vector<Column> columns;
    Column id_col;
    id_col.column_name_ = "id";
    id_col.is_primary_key_ = true;
    id_col.is_nullable_ = false;
    id_col.data_type_ = neon::type::ValueType::Int32;
    id_col.type_len_ = 4;
    id_col.is_unique_ = true;
    columns.push_back(id_col);
    
    auto create_result = mgr.create_table("users", columns);
    ASSERT_TRUE(create_result.first);
    
    // 查询表
    auto query_result = mgr.query_table("users");
    EXPECT_TRUE(query_result.first);
    ASSERT_NE(query_result.second, nullptr);
    EXPECT_EQ(query_result.second->table_name_, "users");
    EXPECT_EQ(query_result.second->columns_.size(), 1);
}

TEST_F(SchemaManagerTest, IndexOperationsTest) {
    // 先创建表
    std::vector<Column> columns;
    Column id_col;
    id_col.column_name_ = "id";
    id_col.is_primary_key_ = true;
    id_col.is_nullable_ = false;
    id_col.data_type_ = neon::type::ValueType::Int32;
    id_col.type_len_ = 4;
    id_col.is_unique_ = true;
    columns.push_back(id_col);
    
    Column name_col;
    name_col.column_name_ = "name";
    name_col.is_primary_key_ = false;
    name_col.is_nullable_ = true;
    name_col.data_type_ = neon::type::ValueType::String;
    name_col.type_len_ = 255;
    name_col.is_unique_ = false;
    columns.push_back(name_col);
    
    auto create_result = mgr.create_table("users", columns);
    ASSERT_TRUE(create_result.first);
    
    // 创建索引
    std::vector<int32_t> index_cols = {1}; // name列
    auto index_result = mgr.create_index("users", "idx_name", index_cols, false);
    EXPECT_TRUE(index_result.first);
    
    // 列出索引
    auto list_result = mgr.list_indexes("users");
    EXPECT_TRUE(list_result.first);
    EXPECT_EQ(list_result.second.size(), 1);
    
    // 删除索引
    auto delete_index_result = mgr.delete_index("users", "idx_name");
    EXPECT_TRUE(delete_index_result.first);
}

TEST_F(SchemaManagerTest, DeleteTableTest) {
    // 先创建表
    std::vector<Column> columns;
    Column id_col;
    id_col.column_name_ = "id";
    id_col.is_primary_key_ = true;
    id_col.is_nullable_ = false;
    id_col.data_type_ = neon::type::ValueType::Int32;
    id_col.type_len_ = 4;
    id_col.is_unique_ = true;
    columns.push_back(id_col);
    
    auto create_result = mgr.create_table("users", columns);
    ASSERT_TRUE(create_result.first);
    
    // 删除表
    auto delete_result = mgr.delete_table("users");
    EXPECT_TRUE(delete_result.first);
}

} // namespace neon::catalog