#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace popo::function
{

// SQL LIKE pattern matching: '%' matches any sequence, '_' matches one character.
bool like_match(std::string_view text, std::string_view pattern) noexcept;

// Compiled LIKE matcher operating on std::string_view without per-row allocations.
struct FastLike
{
    enum class Mode : uint8_t
    {
        Exact,
        Prefix,
        Suffix,
        Contains,
        TwoContains,
        Generic,
    };

    Mode mode = Mode::Generic;
    std::string pat1;
    std::string pat2;
    std::string full_pattern;

    static FastLike compile(std::string_view pattern);
    bool match(std::string_view text) const noexcept;
};

} // namespace popo::function
