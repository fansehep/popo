#include <gtest/gtest.h>
#include "src/type/value.h"

namespace popo::type
{

class ValueTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Setup code if needed
    }

    void TearDown() override
    {
        // Cleanup code if needed
    }
};

TEST_F(ValueTest, IntegerValueTest)
{
    Value val(ValueType::Int8, static_cast<int8_t>(12));
    EXPECT_EQ(val.i8(), 12);

    Value val2(ValueType::Int32, static_cast<int32_t>(42));
    EXPECT_EQ(val2.i32(), 42);
}

TEST_F(ValueTest, StringValueTest)
{
    Value val(ValueType::String, "test_string");
    EXPECT_EQ(val.string_ref(), "test_string");
}

TEST_F(ValueTest, BooleanValueTest)
{
    Value val(ValueType::Bool, true);
    EXPECT_TRUE(val.boolean());

    Value val2(ValueType::Bool, false);
    EXPECT_FALSE(val2.boolean());
}

TEST_F(ValueTest, ValueSizeTest)
{
    // 测试Value对象的大小
    EXPECT_GT(sizeof(Value), 0);
}

TEST_F(ValueTest, ComparisonTest)
{
    Value val1(ValueType::Int32, static_cast<int32_t>(10));
    Value val2(ValueType::Int32, static_cast<int32_t>(20));

    EXPECT_TRUE(val1 < val2);
    EXPECT_FALSE(val1 > val2);
    EXPECT_FALSE(val1 == val2);

    Value val3(ValueType::Int32, static_cast<int32_t>(10));
    EXPECT_TRUE(val1 == val3);
}

} // namespace popo::type