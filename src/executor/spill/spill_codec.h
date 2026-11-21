#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "base/error.h"
#include "type/tuple.h"

namespace popo::executor
{

size_t estimate_value_bytes(const popo::type::Value & value) noexcept;
size_t estimate_row_bytes(const popo::type::Row & row) noexcept;

popo::base::Result<std::vector<char>> encode_spill_row(const popo::type::Row & row);
popo::base::Result<popo::type::Row> decode_spill_row(std::span<const char> data);

} // namespace popo::executor
