#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "base/error.h"
#include "type/value.h"

namespace popo::function
{

using ScalarFn = std::function<popo::base::Result<popo::type::Value>(std::span<const popo::type::Value>)>;

// Registry for built-in and custom SQL scalar functions.
class FunctionRegistry
{
public:
    static FunctionRegistry & instance();

    void register_function(std::string name, ScalarFn fn);
    popo::base::Result<popo::type::Value>
    call(std::string_view name, std::span<const popo::type::Value> args) const;

private:
    FunctionRegistry() = default;

    std::unordered_map<std::string, ScalarFn> custom_functions_;
};

} // namespace popo::function
