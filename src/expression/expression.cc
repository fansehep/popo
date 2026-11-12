#include "src/expression/expression.h"
#include "src/base/error.h"

namespace popo::expression
{

using popo::base::Errno;
using popo::base::make_error;

LiteralExpression::LiteralExpression(const Value & value) : value_(value)
{
}

Result<Value> LiteralExpression::evaluate(const Row & /*row*/)
{
    return value_;
}

popo::type::ValueType LiteralExpression::get_type() const
{
    return value_.datatype();
}

ColumnExpression::ColumnExpression(size_t column_index, popo::type::ValueType type) : column_index_(column_index), type_(type)
{
}

Result<Value> ColumnExpression::evaluate(const Row & row)
{
    if (column_index_ >= row.values_.size())
    {
        return make_error(Errno::InvalidArg, "column index out of range");
    }
    return row.values_[column_index_];
}

popo::type::ValueType ColumnExpression::get_type() const
{
    return type_;
}

BinaryExpression::BinaryExpression(ExpressionRef left, ExpressionRef right) : left_(std::move(left)), right_(std::move(right))
{
}

popo::type::ValueType BinaryExpression::get_type() const
{
    return popo::type::ValueType::Bool;
}

ComparisonExpression::ComparisonExpression(ComparisonType type, ExpressionRef left, ExpressionRef right)
    : BinaryExpression(std::move(left), std::move(right)), type_(type)
{
}

Result<Value> ComparisonExpression::evaluate(const Row & row)
{
    auto left_result = left_->evaluate(row);
    if (!left_result)
    {
        return std::unexpected(left_result.error());
    }

    auto right_result = right_->evaluate(row);
    if (!right_result)
    {
        return std::unexpected(right_result.error());
    }

    const auto & left_value = *left_result;
    const auto & right_value = *right_result;

    if (left_value.datatype() != right_value.datatype())
    {
        return make_error(Errno::InvalidArg, "comparison operand types do not match");
    }

    bool result = false;
    switch (type_)
    {
        case ComparisonType::EQUAL:
            result = (left_value == right_value);
            break;
        case ComparisonType::NOT_EQUAL:
            result = !(left_value == right_value);
            break;
        case ComparisonType::LESS_THAN:
            result = (left_value < right_value);
            break;
        case ComparisonType::LESS_EQUAL:
            result = (left_value < right_value) || (left_value == right_value);
            break;
        case ComparisonType::GREATER_THAN:
            result = !(left_value < right_value) && !(left_value == right_value);
            break;
        case ComparisonType::GREATER_EQUAL:
            result = !(left_value < right_value) || (left_value == right_value);
            break;
    }

    return Value(popo::type::ValueType::Bool, result);
}

popo::type::ValueType ComparisonExpression::get_type() const
{
    return popo::type::ValueType::Bool;
}

ArithmeticExpression::ArithmeticExpression(ArithmeticType type, ExpressionRef left, ExpressionRef right)
    : BinaryExpression(std::move(left), std::move(right)), type_(type)
{
}

Result<Value> ArithmeticExpression::evaluate(const Row & row)
{
    auto left_result = left_->evaluate(row);
    if (!left_result)
    {
        return std::unexpected(left_result.error());
    }

    auto right_result = right_->evaluate(row);
    if (!right_result)
    {
        return std::unexpected(right_result.error());
    }

    const auto & left_value = *left_result;
    const auto & right_value = *right_result;

    if (!left_value.is_numeric() || !right_value.is_numeric())
    {
        return make_error(Errno::InvalidArg, "arithmetic operands must be numeric");
    }

    popo::type::ValueType result_type = left_value.datatype();
    if (right_value.datatype() > result_type)
    {
        result_type = right_value.datatype();
    }

    switch (type_)
    {
        case ArithmeticType::ADD: {
            if (result_type == popo::type::ValueType::Int32)
                return Value(result_type, left_value.i32() + right_value.i32());
            if (result_type == popo::type::ValueType::Int64)
                return Value(result_type, left_value.i64() + right_value.i64());
            if (result_type == popo::type::ValueType::Float32)
                return Value(result_type, left_value.f32() + right_value.f32());
            if (result_type == popo::type::ValueType::Float64)
                return Value(result_type, left_value.f64() + right_value.f64());
            break;
        }
        case ArithmeticType::SUBTRACT: {
            if (result_type == popo::type::ValueType::Int32)
                return Value(result_type, left_value.i32() - right_value.i32());
            if (result_type == popo::type::ValueType::Int64)
                return Value(result_type, left_value.i64() - right_value.i64());
            if (result_type == popo::type::ValueType::Float32)
                return Value(result_type, left_value.f32() - right_value.f32());
            if (result_type == popo::type::ValueType::Float64)
                return Value(result_type, left_value.f64() - right_value.f64());
            break;
        }
        case ArithmeticType::MULTIPLY: {
            if (result_type == popo::type::ValueType::Int32)
                return Value(result_type, left_value.i32() * right_value.i32());
            if (result_type == popo::type::ValueType::Int64)
                return Value(result_type, left_value.i64() * right_value.i64());
            if (result_type == popo::type::ValueType::Float32)
                return Value(result_type, left_value.f32() * right_value.f32());
            if (result_type == popo::type::ValueType::Float64)
                return Value(result_type, left_value.f64() * right_value.f64());
            break;
        }
        case ArithmeticType::DIVIDE: {
            if (result_type == popo::type::ValueType::Int32)
            {
                if (right_value.i32() == 0)
                    return make_error(Errno::InvalidArg, "division by zero");
                return Value(result_type, left_value.i32() / right_value.i32());
            }
            if (result_type == popo::type::ValueType::Int64)
            {
                if (right_value.i64() == 0)
                    return make_error(Errno::InvalidArg, "division by zero");
                return Value(result_type, left_value.i64() / right_value.i64());
            }
            if (result_type == popo::type::ValueType::Float32)
            {
                if (right_value.f32() == 0.0f)
                    return make_error(Errno::InvalidArg, "division by zero");
                return Value(result_type, left_value.f32() / right_value.f32());
            }
            if (result_type == popo::type::ValueType::Float64)
            {
                if (right_value.f64() == 0.0)
                    return make_error(Errno::InvalidArg, "division by zero");
                return Value(result_type, left_value.f64() / right_value.f64());
            }
            break;
        }
    }

    return make_error(Errno::InvalidArg, "unsupported arithmetic type");
}

popo::type::ValueType ArithmeticExpression::get_type() const
{
    return popo::type::ValueType::Int64;
}

LogicalExpression::LogicalExpression(LogicalType type, ExpressionRef left, ExpressionRef right)
    : BinaryExpression(std::move(left), std::move(right)), type_(type)
{
}

Result<Value> LogicalExpression::evaluate(const Row & row)
{
    auto left_result = left_->evaluate(row);
    if (!left_result)
    {
        return std::unexpected(left_result.error());
    }

    if (type_ == LogicalType::AND || type_ == LogicalType::OR)
    {
        auto right_result = right_->evaluate(row);
        if (!right_result)
        {
            return std::unexpected(right_result.error());
        }

        if (left_result->datatype() != popo::type::ValueType::Bool || right_result->datatype() != popo::type::ValueType::Bool)
        {
            return make_error(Errno::InvalidArg, "logical operands must be boolean");
        }

        bool left_bool = left_result->boolean();
        bool right_bool = right_result->boolean();
        bool result = (type_ == LogicalType::AND) ? (left_bool && right_bool) : (left_bool || right_bool);
        return Value(popo::type::ValueType::Bool, result);
    }

    return make_error(Errno::InvalidArg, "unsupported logical operator");
}

popo::type::ValueType LogicalExpression::get_type() const
{
    return popo::type::ValueType::Bool;
}

UnaryExpression::UnaryExpression(ExpressionRef operand) : operand_(std::move(operand))
{
}

popo::type::ValueType UnaryExpression::get_type() const
{
    return popo::type::ValueType::Bool;
}

NotExpression::NotExpression(ExpressionRef operand) : UnaryExpression(std::move(operand))
{
}

Result<Value> NotExpression::evaluate(const Row & row)
{
    auto operand_result = operand_->evaluate(row);
    if (!operand_result)
    {
        return std::unexpected(operand_result.error());
    }

    if (operand_result->datatype() != popo::type::ValueType::Bool)
    {
        return make_error(Errno::InvalidArg, "NOT operand must be boolean");
    }

    return Value(popo::type::ValueType::Bool, !operand_result->boolean());
}

popo::type::ValueType NotExpression::get_type() const
{
    return popo::type::ValueType::Bool;
}

} // namespace popo::expression
