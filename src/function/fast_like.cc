#include "function/fast_like.h"

#include <vector>

namespace popo::function
{

bool like_match(std::string_view text, std::string_view pattern) noexcept
{
    size_t t = 0;
    size_t p = 0;
    size_t star = std::string_view::npos;
    size_t backtrack = 0;
    while (t < text.size())
    {
        if (p < pattern.size() && (pattern[p] == '_' || pattern[p] == text[t]))
        {
            ++t;
            ++p;
        }
        else if (p < pattern.size() && pattern[p] == '%')
        {
            star = p++;
            backtrack = t;
        }
        else if (star != std::string_view::npos)
        {
            p = star + 1;
            t = ++backtrack;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '%')
        ++p;
    return p == pattern.size();
}

FastLike FastLike::compile(std::string_view pattern)
{
    FastLike fl;
    fl.full_pattern = std::string(pattern);
    if (pattern.find('_') != std::string_view::npos)
    {
        fl.mode = Mode::Generic;
        return fl;
    }
    std::vector<size_t> pcts;
    for (size_t i = 0; i < pattern.size(); ++i)
    {
        if (pattern[i] == '%')
            pcts.push_back(i);
    }
    if (pcts.empty())
    {
        fl.mode = Mode::Exact;
        fl.pat1 = std::string(pattern);
    }
    else if (pcts.size() == 1 && pcts[0] == pattern.size() - 1)
    {
        fl.mode = Mode::Prefix;
        fl.pat1 = std::string(pattern.substr(0, pattern.size() - 1));
    }
    else if (pcts.size() == 1 && pcts[0] == 0)
    {
        fl.mode = Mode::Suffix;
        fl.pat1 = std::string(pattern.substr(1));
    }
    else if (pcts.size() == 2 && pcts[0] == 0 && pcts[1] == pattern.size() - 1)
    {
        fl.mode = Mode::Contains;
        fl.pat1 = std::string(pattern.substr(1, pattern.size() - 2));
    }
    else if (pcts.size() == 3 && pcts[0] == 0 && pcts[2] == pattern.size() - 1)
    {
        fl.mode = Mode::TwoContains;
        fl.pat1 = std::string(pattern.substr(1, pcts[1] - 1));
        fl.pat2 = std::string(pattern.substr(pcts[1] + 1, pcts[2] - pcts[1] - 1));
    }
    else
    {
        fl.mode = Mode::Generic;
    }
    return fl;
}

bool FastLike::match(std::string_view text) const noexcept
{
    switch (mode)
    {
        case Mode::Exact:
            return text == pat1;
        case Mode::Prefix:
            return text.starts_with(pat1);
        case Mode::Suffix:
            return text.ends_with(pat1);
        case Mode::Contains:
            return text.find(pat1) != std::string_view::npos;
        case Mode::TwoContains: {
            const size_t p1 = text.find(pat1);
            if (p1 == std::string_view::npos)
                return false;
            return text.find(pat2, p1 + pat1.size()) != std::string_view::npos;
        }
        case Mode::Generic:
            return like_match(text, full_pattern);
    }
    return false;
}

} // namespace popo::function
