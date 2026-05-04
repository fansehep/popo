#pragma once

#include <stdint.h>
#include <string>

namespace neon::base {

struct Error {
    int32_t error_code;
    std::string error_message;
    
    // 构造函数
    Error() {
        error_code = 0;
        error_message = "";
    }
    
    Error(int32_t err_code, const std::string& err_msg) {
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

}  // namespace neon::base

#define NewErr(errno, err_msg, ...) \
    Error(errno, std::format("[{}:{}]" err_msg "", __FILE__, __LINE__, ##__VA_ARGS__))

