#include "function/value_functions.h"

#include <cctype>
#include <cmath>
#include <limits>

namespace popo::function
{

using popo::parser::BinaryOpType;
using popo::parser::ExprType;
using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

namespace
{

std::unexpected<popo::base::Error> function_error(
    std::string message, popo::base::Errno code = popo::base::Errno::InvalidArg)
{
    return popo::base::make_error(code, std::move(message));
}

} // namespace

bool is_null(const Value & v) noexcept
{
    return v.datatype() == ValueType::Null;
}

bool as_bool(const Value & v) noexcept
{
    switch (v.datatype())
    {
        case ValueType::Bool:
            return v.boolean();
        case ValueType::Null:
            return false;
        case ValueType::String:
            return !v.string_ref().empty();
        default:
            return v.is_numeric() && v.to_f64() != 0.0;
    }
}

bool is_floating(const Value & v) noexcept
{
    return v.datatype() == ValueType::Float32 || v.datatype() == ValueType::Float64;
}

bool is_integer_value(const Value & v) noexcept
{
    switch (v.datatype())
    {
        case ValueType::Int8:
        case ValueType::Int16:
        case ValueType::Int32:
        case ValueType::Int64:
        case ValueType::Uint8:
        case ValueType::Uint16:
        case ValueType::Uint32:
        case ValueType::Uint64:
            return true;
        default:
            return false;
    }
}

int64_t to_i64(const Value & v) noexcept
{
    switch (v.datatype())
    {
        case ValueType::Int8:
            return v.i8();
        case ValueType::Int16:
            return v.i16();
        case ValueType::Int32:
            return v.i32();
        case ValueType::Int64:
            return v.i64();
        case ValueType::Uint8:
            return static_cast<int64_t>(v.u8());
        case ValueType::Uint16:
            return static_cast<int64_t>(v.u16());
        case ValueType::Uint32:
            return static_cast<int64_t>(v.u32());
        case ValueType::Uint64:
            return static_cast<int64_t>(v.u64());
        case ValueType::Bool:
            return v.boolean() ? 1 : 0;
        default:
            return static_cast<int64_t>(v.to_f64());
    }
}

Value make_bool(bool b) noexcept
{
    return Value(ValueType::Bool, b);
}

bool comparable(const Value & l, const Value & r) noexcept
{
    return !is_null(l) && !is_null(r) && (l.datatype() == r.datatype() || (l.is_numeric() && r.is_numeric()));
}

bool sql_eq(const Value & l, const Value & r) noexcept
{
    return comparable(l, r) && l == r;
}

bool sql_lt(const Value & l, const Value & r) noexcept
{
    return comparable(l, r) && l < r;
}

bool sql_cmp(BinaryOpType op, const Value & l, const Value & r) noexcept
{
    switch (op)
    {
        case BinaryOpType::Eq:
            return sql_eq(l, r);
        case BinaryOpType::Ne:
            return comparable(l, r) && !sql_eq(l, r);
        case BinaryOpType::Lt:
            return sql_lt(l, r);
        case BinaryOpType::Gt:
            return sql_lt(r, l);
        case BinaryOpType::Le:
            return comparable(l, r) && !sql_lt(r, l);
        case BinaryOpType::Ge:
            return comparable(l, r) && !sql_lt(l, r);
        default:
            return false;
    }
}

popo::base::Result<Value> do_arithmetic(char op, const Value & l, const Value & r)
{
    if (is_null(l) || is_null(r))
        return Value();
    const bool floating = is_floating(l) || is_floating(r);
    switch (op)
    {
        case '+':
            return floating ? Value(ValueType::Float64, l.to_f64() + r.to_f64())
                            : Value(ValueType::Int64, to_i64(l) + to_i64(r));
        case '-':
            return floating ? Value(ValueType::Float64, l.to_f64() - r.to_f64())
                            : Value(ValueType::Int64, to_i64(l) - to_i64(r));
        case '*':
            return floating ? Value(ValueType::Float64, l.to_f64() * r.to_f64())
                            : Value(ValueType::Int64, to_i64(l) * to_i64(r));
        case '/':
            if (r.to_f64() == 0.0)
                return Value();
            if (floating)
                return Value(ValueType::Float64, l.to_f64() / r.to_f64());
            return Value(ValueType::Int64, to_i64(l) / to_i64(r));
        default:
            return function_error("unknown arithmetic operator", popo::base::Errno::InternalErr);
    }
}

bool eq_ci(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string lower_str(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string trimmed_text(std::string_view raw)
{
    size_t begin = 0;
    while (begin < raw.size() && std::isspace(static_cast<unsigned char>(raw[begin])))
        ++begin;
    size_t end = raw.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])))
        --end;
    return std::string(raw.substr(begin, end - begin));
}

popo::base::Result<int64_t> cast_i64(const Value & v)
{
    if (v.datatype() != ValueType::String)
        return to_i64(v);
    std::string text = trimmed_text(v.string_ref());
    if (text.empty())
        return function_error("CAST from empty string");
    auto integer = popo::base::parse_i64(text);
    if (integer)
        return *integer;
    if (integer.error().error_message.find("overflow") != std::string::npos)
        return function_error("CAST overflow");
    auto floating = popo::base::parse_f64(text);
    if (!floating)
        return function_error("CAST could not parse '" + text + "'");
    const long double widened = static_cast<long double>(*floating);
    if (!std::isfinite(*floating)
        || widened < static_cast<long double>(std::numeric_limits<int64_t>::min())
        || widened > static_cast<long double>(std::numeric_limits<int64_t>::max()))
        return function_error("CAST overflow");
    return static_cast<int64_t>(*floating);
}

popo::base::Result<double> cast_f64(const Value & v)
{
    if (v.datatype() != ValueType::String)
        return v.to_f64();
    std::string text = trimmed_text(v.string_ref());
    auto parsed = popo::base::parse_f64(text);
    if (!parsed)
        return function_error("CAST could not parse '" + text + "'");
    return *parsed;
}

popo::base::Result<Value> cast_to(const Value & v, ValueType target)
{
    if (is_null(v))
        return Value();
    switch (target)
    {
        case ValueType::Bool:
            return Value(ValueType::Bool, as_bool(v));
        case ValueType::Int8: {
            auto parsed = cast_i64(v);
            if (!parsed)
                return std::unexpected(parsed.error());
            return Value(ValueType::Int8, static_cast<int8_t>(*parsed));
        }
        case ValueType::Int16: {
            auto parsed = cast_i64(v);
            if (!parsed)
                return std::unexpected(parsed.error());
            return Value(ValueType::Int16, static_cast<int16_t>(*parsed));
        }
        case ValueType::Int32: {
            auto parsed = cast_i64(v);
            if (!parsed)
                return std::unexpected(parsed.error());
            return Value(ValueType::Int32, static_cast<int32_t>(*parsed));
        }
        case ValueType::Int64: {
            auto parsed = cast_i64(v);
            if (!parsed)
                return std::unexpected(parsed.error());
            return Value(ValueType::Int64, *parsed);
        }
        case ValueType::Float32: {
            auto parsed = cast_f64(v);
            if (!parsed)
                return std::unexpected(parsed.error());
            return Value(ValueType::Float32, static_cast<float>(*parsed));
        }
        case ValueType::Float64: {
            auto parsed = cast_f64(v);
            if (!parsed)
                return std::unexpected(parsed.error());
            return Value(ValueType::Float64, *parsed);
        }
        case ValueType::String:
            return Value(
                ValueType::String,
                v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string());
        default:
            return function_error("unsupported CAST target");
    }
}

std::string xml_text(const Value & v)
{
    std::string raw = v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string();
    std::string out;
    for (char c : raw)
    {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out.push_back(c);
    }
    return out;
}

std::string value_key(const Value & v)
{
    if (is_null(v))
        return "N";
    if (v.datatype() == ValueType::Bool)
        return v.boolean() ? "B:1" : "B:0";
    if (v.datatype() == ValueType::String)
        return "S:" + v.string_ref();
    if (v.datatype() == ValueType::Char)
        return std::string("S:") + static_cast<char>(v.u8());
    if (v.is_numeric())
    {
        double d = v.to_f64();
        if (d == 0.0)
            d = 0.0;
        std::string out("D:", 2);
        out.append(reinterpret_cast<const char *>(&d), sizeof(double));
        return out;
    }
    return std::to_string(static_cast<int32_t>(v.datatype())) + ":" + v.to_val_string();
}

std::string row_key(const Row & row)
{
    std::string key;
    for (const auto & v : row.values_)
    {
        key += value_key(v);
        key += '\x1f';
    }
    return key;
}

std::string agg_display_name(const popo::parser::FunctionCallExpr & func)
{
    static const char * kNames[] = {"COUNT", "SUM", "AVG", "MIN", "MAX", "XMLAGG"};
    std::string name = kNames[static_cast<int32_t>(func.func)];
    name += "(";
    if (!func.args.empty() && func.args[0]->type == ExprType::ColumnRef)
    {
        name += static_cast<const popo::parser::ColumnRefExpr &>(*func.args[0]).column_name;
    }
    else if (!func.args.empty() && func.args[0]->type != ExprType::Star)
    {
        name += "...";
    }
    else
    {
        name += "*";
    }
    name += ")";
    return name;
}

} // namespace popo::function
