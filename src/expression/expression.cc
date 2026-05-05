#include "src/expression/expression.h"
#include <stdexcept>
#include "src/base/error.h"

namespace neon::expression
{

// Expression基类不需要实现，因为它是纯虚类

// LiteralExpression实现
LiteralExpression::LiteralExpression(const Value & value) : value_(value)
{
}

std::pair<bool, Value> LiteralExpression::evaluate(const Row & row)
{
    return std::make_pair(true, value_);
}

neon::type::ValueType LiteralExpression::get_type() const
{
    return value_.datatype();
}

// ColumnExpression实现
ColumnExpression::ColumnExpression(size_t column_index, neon::type::ValueType type) : column_index_(column_index), type_(type)
{
}

std::pair<bool, Value> ColumnExpression::evaluate(const Row & row)
{
    if (column_index_ >= row.values_.size())
    {
        // 列索引超出范围
        return std::make_pair(false, Value());
    }

    return std::make_pair(true, row.values_[column_index_]);
}

neon::type::ValueType ColumnExpression::get_type() const
{
    return type_;
}

// BinaryExpression实现
BinaryExpression::BinaryExpression(ExpressionRef left, ExpressionRef right) : left_(std::move(left)), right_(std::move(right))
{
}

neon::type::ValueType BinaryExpression::get_type() const
{
    // 默认返回布尔类型，具体子类可以覆盖
    return neon::type::ValueType::Bool;
}

// ComparisonExpression实现
ComparisonExpression::ComparisonExpression(ComparisonType type, ExpressionRef left, ExpressionRef right)
    : BinaryExpression(std::move(left), std::move(right)), type_(type)
{
}

std::pair<bool, Value> ComparisonExpression::evaluate(const Row & row)
{
    // 计算左右操作数
    auto left_result = left_->evaluate(row);
    if (!left_result.first)
    {
        return std::make_pair(false, Value());
    }

    auto right_result = right_->evaluate(row);
    if (!right_result.first)
    {
        return std::make_pair(false, Value());
    }

    const auto & left_value = left_result.second;
    const auto & right_value = right_result.second;

    // 检查类型是否匹配
    if (left_value.datatype() != right_value.datatype())
    {
        return std::make_pair(false, Value());
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

    return std::make_pair(true, Value(neon::type::ValueType::Bool, result));
}

neon::type::ValueType ComparisonExpression::get_type() const
{
    return neon::type::ValueType::Bool;
}

// ArithmeticExpression实现
ArithmeticExpression::ArithmeticExpression(ArithmeticType type, ExpressionRef left, ExpressionRef right)
    : BinaryExpression(std::move(left), std::move(right)), type_(type)
{
}

std::pair<bool, Value> ArithmeticExpression::evaluate(const Row & row)
{
    // 计算左右操作数
    auto left_result = left_->evaluate(row);
    if (!left_result.first)
    {
        return std::make_pair(false, Value());
    }

    auto right_result = right_->evaluate(row);
    if (!right_result.first)
    {
        return std::make_pair(false, Value());
    }

    const auto & left_value = left_result.second;
    const auto & right_value = right_result.second;

    // 检查是否为数值类型
    if (!left_value.is_numeric() || !right_value.is_numeric())
    {
        return std::make_pair(false, Value());
    }

    // 确定结果类型（取精度较高的类型）
    neon::type::ValueType result_type = left_value.datatype();
    if (right_value.datatype() > result_type)
    {
        result_type = right_value.datatype();
    }

    // 执行算术运算
    switch (type_)
    {
        case ArithmeticType::ADD: {
            if (result_type == neon::type::ValueType::Int32)
            {
                int32_t result = left_value.i32() + right_value.i32();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Int64)
            {
                int64_t result = left_value.i64() + right_value.i64();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Float32)
            {
                float result = left_value.f32() + right_value.f32();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Float64)
            {
                double result = left_value.f64() + right_value.f64();
                return std::make_pair(true, Value(result_type, result));
            }
            break;
        }
        case ArithmeticType::SUBTRACT: {
            if (result_type == neon::type::ValueType::Int32)
            {
                int32_t result = left_value.i32() - right_value.i32();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Int64)
            {
                int64_t result = left_value.i64() - right_value.i64();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Float32)
            {
                float result = left_value.f32() - right_value.f32();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Float64)
            {
                double result = left_value.f64() - right_value.f64();
                return std::make_pair(true, Value(result_type, result));
            }
            break;
        }
        case ArithmeticType::MULTIPLY: {
            if (result_type == neon::type::ValueType::Int32)
            {
                int32_t result = left_value.i32() * right_value.i32();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Int64)
            {
                int64_t result = left_value.i64() * right_value.i64();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Float32)
            {
                float result = left_value.f32() * right_value.f32();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Float64)
            {
                double result = left_value.f64() * right_value.f64();
                return std::make_pair(true, Value(result_type, result));
            }
            break;
        }
        case ArithmeticType::DIVIDE: {
            if (result_type == neon::type::ValueType::Int32)
            {
                if (right_value.i32() == 0)
                {
                    return std::make_pair(false, Value());
                }
                int32_t result = left_value.i32() / right_value.i32();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Int64)
            {
                if (right_value.i64() == 0)
                {
                    return std::make_pair(false, Value());
                }
                int64_t result = left_value.i64() / right_value.i64();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Float32)
            {
                if (right_value.f32() == 0.0f)
                {
                    return std::make_pair(false, Value());
                }
                float result = left_value.f32() / right_value.f32();
                return std::make_pair(true, Value(result_type, result));
            }
            else if (result_type == neon::type::ValueType::Float64)
            {
                if (right_value.f64() == 0.0)
                {
                    return std::make_pair(false, Value());
                }
                double result = left_value.f64() / right_value.f64();
                return std::make_pair(true, Value(result_type, result));
            }
            break;
        }
    }

    return std::make_pair(false, Value());
}

neon::type::ValueType ArithmeticExpression::get_type() const
{
    // 返回数值类型
    return neon::type::ValueType::Int64; // 简化处理，实际应根据操作数类型确定
}

// LogicalExpression实现
LogicalExpression::LogicalExpression(LogicalType type, ExpressionRef left, ExpressionRef right)
    : BinaryExpression(std::move(left), std::move(right)), type_(type)
{
}

std::pair<bool, Value> LogicalExpression::evaluate(const Row & row)
{
    // 计算左操作数
    auto left_result = left_->evaluate(row);
    if (!left_result.first)
    {
        return std::make_pair(false, Value());
    }

    // 对于AND和OR，需要计算右操作数
    if (type_ == LogicalType::AND || type_ == LogicalType::OR)
    {
        auto right_result = right_->evaluate(row);
        if (!right_result.first)
        {
            return std::make_pair(false, Value());
        }

        // 检查操作数是否为布尔类型
        if (left_result.second.datatype() != neon::type::ValueType::Bool || right_result.second.datatype() != neon::type::ValueType::Bool)
        {
            return std::make_pair(false, Value());
        }

        bool left_bool = left_result.second.boolean();
        bool right_bool = right_result.second.boolean();
        bool result = false;

        switch (type_)
        {
            case LogicalType::AND:
                result = left_bool && right_bool;
                break;
            case LogicalType::OR:
                result = left_bool || right_bool;
                break;
            default:
                return std::make_pair(false, Value());
        }

        return std::make_pair(true, Value(neon::type::ValueType::Bool, result));
    }
    else
    {
        // NOT操作已经在NotExpression中实现
        return std::make_pair(false, Value());
    }
}

neon::type::ValueType LogicalExpression::get_type() const
{
    return neon::type::ValueType::Bool;
}

// UnaryExpression实现
UnaryExpression::UnaryExpression(ExpressionRef operand) : operand_(std::move(operand))
{
}

neon::type::ValueType UnaryExpression::get_type() const
{
    return neon::type::ValueType::Bool;
}

// NotExpression实现
NotExpression::NotExpression(ExpressionRef operand) : UnaryExpression(std::move(operand))
{
}

std::pair<bool, Value> NotExpression::evaluate(const Row & row)
{
    auto operand_result = operand_->evaluate(row);
    if (!operand_result.first)
    {
        return std::make_pair(false, Value());
    }

    // 检查操作数是否为布尔类型
    if (operand_result.second.datatype() != neon::type::ValueType::Bool)
    {
        return std::make_pair(false, Value());
    }

    bool operand_bool = operand_result.second.boolean();
    bool result = !operand_bool;

    return std::make_pair(true, Value(neon::type::ValueType::Bool, result));
}

neon::type::ValueType NotExpression::get_type() const
{
    return neon::type::ValueType::Bool;
}

} // namespace neon::expression