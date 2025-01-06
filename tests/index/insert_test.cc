#include <gtest/gtest.h>
#include "src/index/bptree.h"
#include "value.h"

using neon::type::Value;
using neon::type::ValueType;
using neon::type::Row;

TEST(BPTreeTest, InsertTest)
{
    neon::index::BPTree tree;
    Row row;
    row.append_value(Value(ValueType::Int32, 1));
    row.append_value(Value(ValueType::String, "hello"));
    row.append_value(Value(ValueType::Bool, false));
    for (int i = 0; i < 16; i++)
    {
        SPDLOG_INFO("i:{}", i);
        tree.insert(Value(ValueType::Int32, static_cast<int32_t>(i)), row);
    }
    SPDLOG_INFO("{}", tree.to_string());
}
