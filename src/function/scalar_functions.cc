#include "function/scalar_functions.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "function/date_functions.h"
#include "function/value_functions.h"

namespace popo::function
{

using popo::type::Value;
using popo::type::ValueType;

namespace
{

std::unexpected<popo::base::Error> scalar_error(std::string message)
{
    return popo::base::make_error(popo::base::Errno::InvalidArg, std::move(message));
}

std::string text_of(const Value & v)
{
    return v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string();
}

} // namespace

popo::base::Result<Value> eval_scalar_function(std::string_view name, std::span<const Value> args)
{
    if (name == "COALESCE")
    {
        for (const auto & arg : args)
        {
            if (!is_null(arg))
                return arg;
        }
        return Value();
    }
    if (name == "NULLIF")
    {
        if (args.size() != 2)
            return scalar_error("NULLIF expects 2 arguments");
        return sql_eq(args[0], args[1]) ? Value() : args[0];
    }
    if (name == "UPPER" || name == "LOWER")
    {
        if (args.size() != 1)
            return scalar_error(std::string(name) + " expects 1 argument");
        if (is_null(args[0]))
            return Value();
        std::string text = text_of(args[0]);
        for (char & c : text)
        {
            c = static_cast<char>(
                name == "UPPER" ? std::toupper(static_cast<unsigned char>(c))
                                : std::tolower(static_cast<unsigned char>(c)));
        }
        return Value(ValueType::String, text);
    }
    if (name == "CHAR_LENGTH" || name == "CHARACTER_LENGTH" || name == "LENGTH")
    {
        if (args.size() != 1)
            return scalar_error(std::string(name) + " expects 1 argument");
        if (is_null(args[0]))
            return Value();
        return Value(ValueType::Int64, static_cast<int64_t>(text_of(args[0]).size()));
    }
    if (name == "SUBSTRING" || name == "SUBSTR")
    {
        if (args.size() < 2 || is_null(args[0]) || is_null(args[1]))
            return Value();
        std::string text = text_of(args[0]);
        int64_t start = to_i64(args[1]);
        if (start < 1)
            start = 1;
        size_t offset = static_cast<size_t>(start - 1);
        if (offset >= text.size())
            return Value(ValueType::String, std::string());
        size_t len = text.size() - offset;
        if (args.size() >= 3 && !is_null(args[2]))
        {
            int64_t asked = to_i64(args[2]);
            if (asked < 0)
                asked = 0;
            len = std::min(len, static_cast<size_t>(asked));
        }
        return Value(ValueType::String, text.substr(offset, len));
    }
    if (name == "TRIM")
    {
        if (args.empty() || is_null(args[0]))
            return Value();
        std::string text = text_of(args[0]);
        std::string mode = "BOTH";
        std::string chars = " ";
        if (args.size() >= 3)
        {
            if (is_null(args[2]))
                return Value();
            mode = text_of(args[1]);
            chars = text_of(args[2]);
        }
        if (chars.empty())
            return Value(ValueType::String, text);
        size_t begin = 0;
        if (mode == "LEADING" || mode == "BOTH")
        {
            while (begin < text.size() && chars.find(text[begin]) != std::string::npos)
                ++begin;
        }
        size_t end = text.size();
        if (mode == "TRAILING" || mode == "BOTH")
        {
            while (end > begin && chars.find(text[end - 1]) != std::string::npos)
                --end;
        }
        return Value(ValueType::String, text.substr(begin, end - begin));
    }
    if (name == "LTRIM" || name == "RTRIM")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        std::string text = text_of(args[0]);
        size_t begin = 0;
        size_t end = text.size();
        if (name == "LTRIM")
        {
            while (begin < end && text[begin] == ' ')
                ++begin;
        }
        else
        {
            while (end > begin && text[end - 1] == ' ')
                --end;
        }
        return Value(ValueType::String, text.substr(begin, end - begin));
    }
    if (name == "EXTRACT")
    {
        if (args.size() != 2 || is_null(args[0]) || is_null(args[1]))
            return Value();
        const std::string_view field_raw = args[0].datatype() == ValueType::String
            ? std::string_view(args[0].string_ref()) : std::string_view();
        std::string field_owned;
        std::string_view field_sv = field_raw;
        if (args[0].datatype() != ValueType::String)
        {
            field_owned = text_of(args[0]);
            field_sv = field_owned;
        }
        std::string date_owned;
        const std::string_view date_sv = (args[1].datatype() == ValueType::String)
            ? std::string_view(args[1].string_ref())
            : std::string_view(date_owned = text_of(args[1]));
        int y = 0, m = 0, d = 0;
        if (!parse_ymd(date_sv, y, m, d))
            return scalar_error("EXTRACT: invalid date '" + std::string(date_sv) + "'");
        if (eq_ci(field_sv, "YEAR"))
            return Value(ValueType::Int64, static_cast<int64_t>(y));
        if (eq_ci(field_sv, "MONTH"))
            return Value(ValueType::Int64, static_cast<int64_t>(m));
        if (eq_ci(field_sv, "DAY"))
            return Value(ValueType::Int64, static_cast<int64_t>(d));
        return scalar_error("EXTRACT: unsupported field '" + std::string(field_sv) + "'");
    }
    if (name == "YEAR" || name == "MONTH" || name == "DAY")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        std::string date_owned;
        const std::string_view date_sv = (args[0].datatype() == ValueType::String)
            ? std::string_view(args[0].string_ref())
            : std::string_view(date_owned = text_of(args[0]));
        int y = 0, m = 0, d = 0;
        if (!parse_ymd(date_sv, y, m, d))
            return scalar_error(std::string(name) + ": invalid date '" + std::string(date_sv) + "'");
        if (name == "YEAR")
            return Value(ValueType::Int64, static_cast<int64_t>(y));
        if (name == "MONTH")
            return Value(ValueType::Int64, static_cast<int64_t>(m));
        return Value(ValueType::Int64, static_cast<int64_t>(d));
    }
    if (name == "ROUND")
    {
        if (args.empty() || is_null(args[0]))
            return Value();
        const int64_t scale = (args.size() >= 2 && !is_null(args[1])) ? to_i64(args[1]) : 0;
        const double factor = std::pow(10.0, static_cast<double>(scale));
        return Value(ValueType::Float64, std::round(args[0].to_f64() * factor) / factor);
    }
    if (name == "FLOOR")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        return Value(ValueType::Float64, std::floor(args[0].to_f64()));
    }
    if (name == "CEIL" || name == "CEILING")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        return Value(ValueType::Float64, std::ceil(args[0].to_f64()));
    }
    if (name == "MOD")
    {
        if (args.size() != 2 || is_null(args[0]) || is_null(args[1]))
            return Value();
        if (is_floating(args[0]) || is_floating(args[1]))
        {
            const double b = args[1].to_f64();
            return b == 0.0 ? Value() : Value(ValueType::Float64, std::fmod(args[0].to_f64(), b));
        }
        const int64_t b = to_i64(args[1]);
        return b == 0 ? Value() : Value(ValueType::Int64, to_i64(args[0]) % b);
    }
    if (name == "ABS")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        if (is_floating(args[0]))
            return Value(ValueType::Float64, std::fabs(args[0].to_f64()));
        const int64_t n = to_i64(args[0]);
        return Value(ValueType::Int64, n < 0 ? -n : n);
    }
    if (name == "CONCAT")
    {
        std::string text;
        for (const auto & arg : args)
        {
            if (is_null(arg))
                return Value();
            text += text_of(arg);
        }
        return Value(ValueType::String, text);
    }
    if (name == "XMLCOMMENT")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        return Value(ValueType::String, "<!--" + text_of(args[0]) + "-->");
    }
    if (name == "XMLCONCAT")
    {
        std::string text;
        for (const auto & arg : args)
        {
            if (!is_null(arg))
                text += text_of(arg);
        }
        return Value(ValueType::String, text);
    }
    if (name == "XMLELEMENT")
    {
        if (args.empty())
            return scalar_error("XMLELEMENT requires a name");
        std::string tag = text_of(args[0]);
        std::string body = args.size() > 1 && !is_null(args[1]) ? xml_text(args[1]) : "";
        return Value(ValueType::String, "<" + tag + ">" + body + "</" + tag + ">");
    }
    if (name == "XMLFOREST")
    {
        std::string text;
        for (size_t i = 0; i + 1 < args.size(); i += 2)
        {
            if (is_null(args[i + 1]))
                continue;
            std::string tag = text_of(args[i]);
            text += "<" + tag + ">" + xml_text(args[i + 1]) + "</" + tag + ">";
        }
        return Value(ValueType::String, text);
    }
    return scalar_error("unknown function " + std::string(name));
}

} // namespace popo::function
