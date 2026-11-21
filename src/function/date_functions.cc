#include "function/date_functions.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace popo::function
{

bool parse_ymd(std::string_view text, int & y, int & m, int & d) noexcept
{
    size_t p = 0;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p])))
        ++p;
    auto read_int = [&](int & out) -> bool
    {
        if (p >= text.size() || !std::isdigit(static_cast<unsigned char>(text[p])))
            return false;
        int v = 0;
        while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p])))
        {
            v = v * 10 + (text[p] - '0');
            ++p;
        }
        out = v;
        return true;
    };
    if (!read_int(y) || p >= text.size() || text[p] != '-')
        return false;
    ++p;
    if (!read_int(m) || p >= text.size() || text[p] != '-')
        return false;
    ++p;
    if (!read_int(d))
        return false;
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

bool is_leap_year(int y) noexcept
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int days_in_month(int y, int m) noexcept
{
    static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && is_leap_year(y))
        return 29;
    return (m >= 1 && m <= 12) ? kDays[m] : 30;
}

int64_t days_from_civil(int y, int m, int d) noexcept
{
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (static_cast<unsigned>(m) + (m > 2 ? static_cast<unsigned>(-3) : 9u)) + 2) / 5
        + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int & y, int & m, int & d) noexcept
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2 ? 1 : 0;
}

popo::base::Result<std::string>
add_date_interval(std::string_view raw, int64_t delta, popo::parser::IntervalUnit unit)
{
    int y = 0;
    int m = 0;
    int d = 0;
    if (!parse_ymd(raw, y, m, d))
    {
        return popo::base::make_error(popo::base::Errno::InvalidArg, "invalid date '" + std::string(raw) + "'");
    }
    switch (unit)
    {
        case popo::parser::IntervalUnit::Year:
            y = static_cast<int>(y + delta);
            d = std::min(d, days_in_month(y, m));
            break;
        case popo::parser::IntervalUnit::Month: {
            int64_t total = static_cast<int64_t>(y) * 12 + (m - 1) + delta;
            int64_t ny = total >= 0 ? total / 12 : (total - 11) / 12;
            int nm = static_cast<int>(total - ny * 12) + 1;
            y = static_cast<int>(ny);
            m = nm;
            d = std::min(d, days_in_month(y, m));
            break;
        }
        case popo::parser::IntervalUnit::Day: {
            int64_t days = days_from_civil(y, m, d) + delta;
            civil_from_days(days, y, m, d);
            break;
        }
    }
    return std::format("{:04d}-{:02d}-{:02d}", y, m, d);
}

} // namespace popo::function
