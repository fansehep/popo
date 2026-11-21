#include "function/function_registry.h"

#include "function/scalar_functions.h"

namespace popo::function
{

FunctionRegistry & FunctionRegistry::instance()
{
    static FunctionRegistry registry;
    return registry;
}

void FunctionRegistry::register_function(std::string name, ScalarFn fn)
{
    custom_functions_[std::move(name)] = std::move(fn);
}

popo::base::Result<popo::type::Value>
FunctionRegistry::call(std::string_view name, std::span<const popo::type::Value> args) const
{
    if (!custom_functions_.empty())
    {
        auto it = custom_functions_.find(std::string(name));
        if (it != custom_functions_.end())
            return it->second(args);
    }
    return eval_scalar_function(name, args);
}

} // namespace popo::function
