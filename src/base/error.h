#pragma once

#include <expected>
#include <format>
#include <string>
#include <stdint.h>

namespace neon::base
{

struct Error
{
    int32_t error_code;
    std::string error_message;

    // 构造函数
    Error()
    {
        error_code = 0;
        error_message = "";
    }

    Error(int32_t err_code, const std::string & err_msg)
    {
        error_code = err_code;
        error_message = err_msg;
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
};

// Convenience alias for std::expected<T, neon::base::Error>
template <typename T>
using Result = std::expected<T, neon::base::Error>;

} // namespace neon::base

// Convenience helper: creates a std::unexpected<Error> with file/line info
#define NewErr(errno, err_msg, ...) \
    std::unexpected(neon::base::Error(errno, std::format("[{}:{}]" err_msg "", __FILE__, __LINE__, ##__VA_ARGS__)))
