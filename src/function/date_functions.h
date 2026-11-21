#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "base/error.h"
#include "parser/ast.h"

namespace popo::function
{

bool parse_ymd(std::string_view text, int & y, int & m, int & d) noexcept;
bool is_leap_year(int y) noexcept;
int days_in_month(int y, int m) noexcept;
int64_t days_from_civil(int y, int m, int d) noexcept;
void civil_from_days(int64_t z, int & y, int & m, int & d) noexcept;

popo::base::Result<std::string>
add_date_interval(std::string_view raw, int64_t delta, popo::parser::IntervalUnit unit);

// Pack a 10-byte ISO date string into big-endian (hi, lo) for exact lexicographical SIMD comparison.
inline void pack_str10(const char * ptr, uint64_t & hi, uint16_t & lo) noexcept
{
    uint64_t raw_hi = 0;
    uint16_t raw_lo = 0;
    std::memcpy(&raw_hi, ptr, 8);
    std::memcpy(&raw_lo, ptr + 8, 2);
    hi = __builtin_bswap64(raw_hi);
    lo = __builtin_bswap16(raw_lo);
}

} // namespace popo::function
