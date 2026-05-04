#include "src/expression/expression.h"
#include "src/type/tuple.h"
#include <gtest/gtest.h>
#include <memory>

namespace neon::expression {

class ExpressionTest : public ::testing::Test {
protected:
    neon::type::Row test_row;
    
    void SetUp() override {
        // 创建测试数据行
        test_row.append_value(neon::type::Value(neon::type::ValueType::Int32, 10));
        test_row.append_value(neon::type::Value(neon::type::ValueType::Int32, 20));
        test_row.append_value(neon::type::Value(neon::type::ValueType::Bool, true));
        test_row.append_value(neon::type::Value(neon::type::ValueType::Bool, false));
    }
    
    void TearDown() override {
        // Cleanup code if needed
    }
};

TEST_F(ExpressionTest, LiteralExpressionTest) {
    auto literal_expr = std::make_shared<LiteralExpression>(
        neon::type::Value(neon::type::ValueType::Int32, 42));
    auto literal_result = literal_expr->evaluate(test_row);
    EXPECT_TRUE(literal_result.first);
    EXPECT_EQ(literal_result.second.datatype(), neon::type::ValueType::Int32);
    EXPECT_EQ(literal_result.second.i32(), 42);
}

TEST_F(ExpressionTest, ColumnExpressionTest) {
    auto column_expr = std::make_shared<ColumnExpression>(0, neon::type::ValueType::Int32);
    auto column_result = column_expr->evaluate(test_row);
    EXPECT_TRUE(column_result.first);
    EXPECT_EQ(column_result.second.datatype(), neon::type::ValueType::Int32);
    EXPECT_EQ(column_result.second.i32(), 10);
}

TEST_F(ExpressionTest, ComparisonExpressionTest) {
    auto left_column = std::make_shared<ColumnExpression>(0, neon::type::ValueType::Int32);
    auto right_column = std::make_shared<ColumnExpression>(1, neon::type::ValueType::Int32);
    auto comparison_expr = std::make_shared<ComparisonExpression>(
        ComparisonType::LESS_THAN, left_column, right_column);
    auto comparison_result = comparison_expr->evaluate(test_row);
    EXPECT_TRUE(comparison_result.first);
    EXPECT_EQ(comparison_result.second.datatype(), neon::type::ValueType::Bool);
    EXPECT_TRUE(comparison_result.second.boolean());
}

TEST_F(ExpressionTest, ArithmeticExpressionTest) {
    auto left_column = std::make_shared<ColumnExpression>(0, neon::type::ValueType::Int32);
    auto right_column = std::make_shared<ColumnExpression>(1, neon::type::ValueType::Int32);
    auto arithmetic_expr = std::make_shared<ArithmeticExpression>(
        ArithmeticType::ADD, left_column, right_column);
    auto arithmetic_result = arithmetic_expr->evaluate(test_row);
    EXPECT_TRUE(arithmetic_result.first);
    EXPECT_EQ(arithmetic_result.second.datatype(), neon::type::ValueType::Int32);
    EXPECT_EQ(arithmetic_result.second.i32(), 30);
}

TEST_F(ExpressionTest, LogicalExpressionTest) {
    auto left_bool = std::make_shared<ColumnExpression>(2, neon::type::ValueType::Bool);
    auto right_bool = std::make_shared<ColumnExpression>(3, neon::type::ValueType::Bool);
    auto logical_expr = std::make_shared<LogicalExpression>(
        LogicalType::AND, left_bool, right_bool);
    auto logical_result = logical_expr->evaluate(test_row);
    EXPECT_TRUE(logical_result.first);
    EXPECT_EQ(logical_result.second.datatype(), neon::type::ValueType::Bool);
    EXPECT_FALSE(logical_result.second.boolean());
}

TEST_F(ExpressionTest, NotExpressionTest) {
    auto bool_expr = std::make_shared<ColumnExpression>(2, neon::type::ValueType::Bool);
    auto not_expr = std::make_shared<NotExpression>(bool_expr);
    auto not_result = not_expr->evaluate(test_row);
    EXPECT_TRUE(not_result.first);
    EXPECT_EQ(not_result.second.datatype(), neon::type::ValueType::Bool);
    EXPECT_FALSE(not_result.second.boolean());
}

} // namespace neon::expression