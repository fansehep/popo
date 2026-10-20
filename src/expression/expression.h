#pragma once

#include <memory>
#include <string>
#include "src/base/error.h"
#include "src/type/tuple.h"
#include "src/type/value.h"

namespace popo::expression
{

using popo::base::Error;
using popo::type::Row;
using popo::type::Value;

// 表达式基类
class Expression
{
public:
    virtual ~Expression() = default;

    // 计算表达式的值
    virtual std::pair<bool, Value> evaluate(const Row & row) = 0;

    // 获取表达式的类型
    virtual popo::type::ValueType get_type() const = 0;
};

using ExpressionRef = std::shared_ptr<Expression>;

// 字面量表达式
class LiteralExpression : public Expression
{
public:
    explicit LiteralExpression(const Value & value);

    std::pair<bool, Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;

private:
    Value value_;
};

// 列引用表达式
class ColumnExpression : public Expression
{
public:
    ColumnExpression(size_t column_index, popo::type::ValueType type);

    std::pair<bool, Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;

private:
    size_t column_index_;
    popo::type::ValueType type_;
};

// 二元操作表达式基类
class BinaryExpression : public Expression
{
public:
    BinaryExpression(ExpressionRef left, ExpressionRef right);

    popo::type::ValueType get_type() const override;

protected:
    ExpressionRef left_;
    ExpressionRef right_;
};

// 比较操作表达式
enum class ComparisonType
{
    EQUAL,
    NOT_EQUAL,
    LESS_THAN,
    LESS_EQUAL,
    GREATER_THAN,
    GREATER_EQUAL
};

class ComparisonExpression : public BinaryExpression
{
public:
    ComparisonExpression(ComparisonType type, ExpressionRef left, ExpressionRef right);

    std::pair<bool, Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;

private:
    ComparisonType type_;
};

// 算术操作表达式
enum class ArithmeticType
{
    ADD,
    SUBTRACT,
    MULTIPLY,
    DIVIDE
};

class ArithmeticExpression : public BinaryExpression
{
public:
    ArithmeticExpression(ArithmeticType type, ExpressionRef left, ExpressionRef right);

    std::pair<bool, Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;

private:
    ArithmeticType type_;
};

// 逻辑操作表达式
enum class LogicalType
{
    AND,
    OR,
    NOT
};

class LogicalExpression : public BinaryExpression
{
public:
    LogicalExpression(LogicalType type, ExpressionRef left, ExpressionRef right);

    std::pair<bool, Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;

private:
    LogicalType type_;
};

// 一元操作表达式基类
class UnaryExpression : public Expression
{
public:
    explicit UnaryExpression(ExpressionRef operand);

    popo::type::ValueType get_type() const override;

protected:
    ExpressionRef operand_;
};

// 逻辑非表达式
class NotExpression : public UnaryExpression
{
public:
    explicit NotExpression(ExpressionRef operand);

    std::pair<bool, Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;
};

} // namespace popo::expression