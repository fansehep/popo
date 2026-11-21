#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "base/error.h"
#include "parser/ast.h"
#include "type/tuple.h"
#include "type/value.h"

namespace popo::function
{

bool is_null(const popo::type::Value & v) noexcept;
bool as_bool(const popo::type::Value & v) noexcept;
bool is_floating(const popo::type::Value & v) noexcept;
bool is_integer_value(const popo::type::Value & v) noexcept;
int64_t to_i64(const popo::type::Value & v) noexcept;
popo::type::Value make_bool(bool b) noexcept;

bool comparable(const popo::type::Value & l, const popo::type::Value & r) noexcept;
bool sql_eq(const popo::type::Value & l, const popo::type::Value & r) noexcept;
bool sql_lt(const popo::type::Value & l, const popo::type::Value & r) noexcept;
bool sql_cmp(popo::parser::BinaryOpType op, const popo::type::Value & l, const popo::type::Value & r) noexcept;

popo::base::Result<popo::type::Value>
do_arithmetic(char op, const popo::type::Value & l, const popo::type::Value & r);

bool eq_ci(std::string_view a, std::string_view b) noexcept;
std::string lower_str(std::string_view s);
std::string trimmed_text(std::string_view raw);

popo::base::Result<int64_t> cast_i64(const popo::type::Value & v);
popo::base::Result<double> cast_f64(const popo::type::Value & v);
popo::base::Result<popo::type::Value> cast_to(const popo::type::Value & v, popo::type::ValueType target);

std::string xml_text(const popo::type::Value & v);
std::string value_key(const popo::type::Value & v);
std::string row_key(const popo::type::Row & row);
std::string agg_display_name(const popo::parser::FunctionCallExpr & func);

} // namespace popo::function
