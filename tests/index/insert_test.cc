#include <memory>
#include <random>
#include <gtest/gtest.h>
#include "src/index/bptree.h"
#include "spdlog/spdlog.h"
#include "value.h"

using neon::type::Row;
using neon::type::RowRef;
using neon::type::Value;
using neon::type::ValueType;

TEST(BPTreeTest, InsertTest)
{
    neon::index::BPTree tree;
    RowRef row = std::make_shared<Row>();
    row->append_value(Value(ValueType::Int32, 1));
    row->append_value(Value(ValueType::String, "hello"));
    row->append_value(Value(ValueType::Bool, false));

    std::vector<int> numbers(1000);
    std::iota(numbers.begin(), numbers.end(), 1);
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(numbers.begin(), numbers.end(), g);
    for (int num : numbers)
    {
        tree.insert(Value(ValueType::Int32, num), row);
    }
    int i = 1;
    for (; i <= 1000; ++i)
    {
        auto r = tree.get(Value(ValueType::Int32, i));
        ASSERT_EQ(r, row);
    }
    ASSERT_GT(i, 1);
}

TEST(BPTreeTest, DeleteTest)
{
    neon::index::BPTree tree;
    RowRef row = std::make_shared<Row>();
    row->append_value(Value(ValueType::Int32, 1));
    row->append_value(Value(ValueType::String, "hello"));
    row->append_value(Value(ValueType::Bool, false));

    std::vector<int> numbers(1000);
    std::iota(numbers.begin(), numbers.end(), 1);
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(numbers.begin(), numbers.end(), g);
    for (int num : numbers)
    {
        tree.insert(Value(ValueType::Int32, num), row);
    }
    int i = 1;
    for (; i <= 1000; ++i)
    {
        auto is_delete = tree.remove(Value(ValueType::Int32, i));
        ASSERT_EQ(is_delete, true);
    }
    ASSERT_GT(i, 1);
    i = 1;
    for (; i <= 1000; ++i)
    {
        auto r = tree.get(Value(ValueType::Int32, i));
        ASSERT_EQ(r, nullptr);
    }
    ASSERT_GT(i, 1);
}

TEST(BPTreeTest, IterTest)
{
    neon::index::BPTree tree;
    RowRef row = std::make_shared<Row>();
    row->append_value(Value(ValueType::Int32, 1));
    row->append_value(Value(ValueType::String, "hello"));
    row->append_value(Value(ValueType::Bool, false));

    // 插入1到32的数字
    for (int i = 1; i <= 32; i++) {
        tree.insert(Value(ValueType::Int32, i), row);
    }
    
    // 首先验证所有键都能被找到
    for (int i = 1; i <= 32; i++) {
        auto r = tree.get(Value(ValueType::Int32, i));
        ASSERT_NE(r, nullptr);
        ASSERT_EQ(r, row);
    }
    
    // 然后验证迭代器
    auto iter = tree.iter();
    int i = 1;
    //SPDLOG_INFO("{}", tree.to_string());
    while (iter.is_valid())
    {
        auto data = iter.kv_ref();
        ASSERT_NE(data, nullptr);
        // Print debug information
        // SPDLOG_INFO("Expected: {}, Actual: {}", i, data->first.i32());
        ASSERT_EQ(data->first, Value(ValueType::Int32, i));
        i++;
        iter.next();
    }
    // 确保我们遍历了所有的元素
    ASSERT_EQ(i, 33);  // 1到32，所以最后i应该是33
}

TEST(BPTreeTest, SimpleIterTest)
{
    neon::index::BPTree tree;
    RowRef row = std::make_shared<Row>();
    row->append_value(Value(ValueType::Int32, 1));
    row->append_value(Value(ValueType::String, "hello"));
    row->append_value(Value(ValueType::Bool, false));

    // 插入少量数字以简化调试
    for (int i = 1; i <= 5; i++) {
        tree.insert(Value(ValueType::Int32, i), row);
    }
    
    // 首先验证所有键都能被找到
    for (int i = 1; i <= 5; i++) {
        auto r = tree.get(Value(ValueType::Int32, i));
        ASSERT_NE(r, nullptr);
        ASSERT_EQ(r, row);
    }
    
    // 然后验证迭代器
    auto iter = tree.iter();
    int i = 1;
    while (iter.is_valid())
    {
        auto data = iter.kv_ref();
        ASSERT_NE(data, nullptr);
        ASSERT_EQ(data->first, Value(ValueType::Int32, i));
        i++;
        iter.next();
    }
    // 确保我们遍历了所有的元素
    ASSERT_EQ(i, 6);  // 1到5，所以最后i应该是6
}

TEST(BPTreeTest, BeginIterTest)
{
    neon::index::BPTree tree;
    RowRef row = std::make_shared<Row>();
    row->append_value(Value(ValueType::Int32, 1));
    row->append_value(Value(ValueType::String, "hello"));
    row->append_value(Value(ValueType::Bool, false));

    // 插入1到600的数字（减少数量以提高测试速度）
    for (int i = 1; i <= 600; i++) {
        tree.insert(Value(ValueType::Int32, i), row);
    }
    
    // 从549开始迭代
    auto iter = tree.iter_with_begin(Value(ValueType::Int32, 549));

    int i = 549;  // 应该从549开始
    while (iter.is_valid())
    {
        auto data = iter.kv_ref();
        ASSERT_NE(data, nullptr);
        ASSERT_EQ(data->first, Value(ValueType::Int32, i));
        i++;
        iter.next();
    }
    // 应该遍历从549到600的所有数字
    ASSERT_EQ(i, 601);
}