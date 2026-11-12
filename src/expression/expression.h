#pragma once

#include <memory>
#include <string>
#include "src/base/error.h"
#include "src/type/tuple.h"
#include "src/type/value.h"

namespace popo::expression
{

using popo::base::Error;
using popo::base::Result;
using popo::type::Row;
using popo::type::Value;

class Expression
{
public:
    virtual ~Expression() = default;

    virtual Result<Value> evaluate(const Row & row) = 0;

    virtual popo::type::ValueType get_type() const = 0;
};

using ExpressionRef = std::shared_ptr<Expression>;

class LiteralExpression : public Expression
{
public:
    explicit LiteralExpression(const Value & value);

    Result<Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;

private:
    Value value_;
};

class ColumnExpression : public Expression
{
public:
    ColumnExpression(size_t column_index, popo::type::ValueType type);

    Result<Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;

private:
    size_t column_index_;
    popo::type::ValueType type_;
};

class BinaryExpression : public Expression
{
public:
    BinaryExpression(ExpressionRef left, ExpressionRef right);

    popo::type::ValueType get_type() const override;

protected:
    ExpressionRef left_;
    ExpressionRef right_;
};

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

    Result<Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;

private:
    ComparisonType type_;
};

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

    Result<Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;

private:
    ArithmeticType type_;
};

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

    Result<Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;

private:
    LogicalType type_;
};

class UnaryExpression : public Expression
{
public:
    explicit UnaryExpression(ExpressionRef operand);

    popo::type::ValueType get_type() const override;

protected:
    ExpressionRef operand_;
};

class NotExpression : public UnaryExpression
{
public:
    explicit NotExpression(ExpressionRef operand);

    Result<Value> evaluate(const Row & row) override;

    popo::type::ValueType get_type() const override;
};

} // namespace popo::expression
