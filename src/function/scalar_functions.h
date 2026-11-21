#pragma once

#include <span>
#include <string_view>

#include "base/error.h"
#include "type/value.h"

namespace popo::function
{

popo::base::Result<popo::type::Value>
eval_scalar_function(std::string_view name, std::span<const popo::type::Value> args);

} // namespace popo::function
