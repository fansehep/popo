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

    std::vector<int> numbers(32);
    std::iota(numbers.begin(), numbers.end(), 1);
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(numbers.begin(), numbers.end(), g);
    for (int num : numbers)
    {
        tree.insert(Value(ValueType::Int32, num), row);
    }
    auto iter = tree.iter();
    int i = 1;
    SPDLOG_INFO("{}", tree.to_string());
    for (; iter.is_valid(); iter.next())
    {
        auto data = iter.kv_ref();
        ASSERT_EQ(data->first, Value(ValueType::Int32, i));
        i++;
    }
    ASSERT_GT(i, 1);
}

TEST(BPTreeTest, BeginIterTest)
{
    neon::index::BPTree tree;
    RowRef row = std::make_shared<Row>();
    row->append_value(Value(ValueType::Int32, 1));
    row->append_value(Value(ValueType::String, "hello"));
    row->append_value(Value(ValueType::Bool, false));

    std::vector<int> numbers(100000);
    std::iota(numbers.begin(), numbers.end(), 1);
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(numbers.begin(), numbers.end(), g);
    for (int num : numbers)
    {
        tree.insert(Value(ValueType::Int32, num), row);
    }
    auto iter = tree.iter_with_begin(Value(ValueType::Int32, 549));

    int i = 1;
    for (; iter.is_valid(); iter.next())
    {
        auto data = iter.kv_ref();
        ASSERT_EQ(data->first, Value(ValueType::Int32, 1));
        i++;
    }
    ASSERT_GT(i, 1);
}