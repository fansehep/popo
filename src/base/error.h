#pragma once

#include <cctype>
#include <charconv>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace popo::base
{

struct Error
{
    int32_t error_code = 0;
    std::string error_message;

    Error() = default;

    Error(int32_t err_code, std::string err_msg) : error_code(err_code), error_message(std::move(err_msg))
    {
    }
};

enum Errno : int32_t
{
    InternalErr,
    InvalidArg,
    Unknown,
    TableNotFound,
    TableAlreadyExists,
    InvalidColumnDefinition,
    IoErr,
    DuplicateKey,
    SerializationFailure,
    AuthDenied,
    ProtocolErr,
    EndOfStream,
};

// Unified error-handling type across PopoDB: std::expected<T, Error>.
template <typename T>
using Result = std::expected<T, popo::base::Error>;

inline std::unexpected<Error> make_error(Errno code, std::string msg)
{
    return std::unexpected(Error(static_cast<int32_t>(code), std::move(msg)));
}

inline std::unexpected<Error> make_error(int32_t code, std::string msg)
{
    return std::unexpected(Error(code, std::move(msg)));
}

inline std::string_view trim_ascii(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    {
        text.remove_suffix(1);
    }
    return text;
}

inline Result<int64_t> parse_i64(std::string_view raw)
{
    std::string_view text = trim_ascii(raw);
    if (text.empty())
    {
        return make_error(Errno::InvalidArg, "empty integer string");
    }
    if (text.front() == '+' && text.size() > 1 && text[1] != '+' && text[1] != '-')
    {
        text.remove_prefix(1);
    }
    int64_t value = 0;
    const char * begin = text.data();
    const char * end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value, 10);
    if (ec == std::errc::result_out_of_range)
    {
        return make_error(Errno::InvalidArg, std::format("integer overflow: '{}'", raw));
    }
    if (ec != std::errc{} || ptr != end)
    {
        return make_error(Errno::InvalidArg, std::format("invalid integer: '{}'", raw));
    }
    return value;
}

inline Result<double> parse_f64(std::string_view raw)
{
    std::string_view text = trim_ascii(raw);
    if (text.empty())
    {
        return make_error(Errno::InvalidArg, "empty float string");
    }
    if (text.front() == '+' && text.size() > 1 && text[1] != '+' && text[1] != '-')
    {
        text.remove_prefix(1);
    }
    double value = 0.0;
    const char * begin = text.data();
    const char * end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec == std::errc::result_out_of_range)
    {
        return make_error(Errno::InvalidArg, std::format("float overflow: '{}'", raw));
    }
    if (ec != std::errc{} || ptr != end)
    {
        return make_error(Errno::InvalidArg, std::format("invalid float: '{}'", raw));
    }
    return value;
}

inline Result<uint16_t> parse_u16(std::string_view raw)
{
    auto parsed = parse_i64(raw);
    if (!parsed)
    {
        return std::unexpected(parsed.error());
    }
    if (*parsed < 0 || *parsed > 65535)
    {
        return make_error(Errno::InvalidArg, std::format("uint16 out of range: '{}'", raw));
    }
    return static_cast<uint16_t>(*parsed);
}

} // namespace popo::base

#define POPO_TRY(expr)                                                \
    do                                                                \
    {                                                                 \
        if (auto _popo_try_res = (expr); !_popo_try_res.has_value())  \
        {                                                             \
            return std::unexpected(std::move(_popo_try_res).error()); \
        }                                                             \
    } while (0)
