#pragma once

// Internal ASCII helpers. HTTP syntax is defined over bytes, so these
// deliberately ignore the C locale that <cctype> consults.

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace http::detail {

constexpr char toLower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a, b, [](char x, char y) { return toLower(x) == toLower(y); });
}

constexpr bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

constexpr bool isHexDigit(char c) noexcept
{
    return isDigit(c) || (toLower(c) >= 'a' && toLower(c) <= 'f');
}

// A token character (tchar) from RFC 9110, section 5.6.2.
constexpr bool isTchar(char c) noexcept
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c)) {
        return true;
    }
    return std::string_view{"!#$%&'*+-.^_`|~"}.contains(c);
}

constexpr bool isToken(std::string_view text) noexcept
{
    return !text.empty() && std::ranges::all_of(text, isTchar);
}

// Optional whitespace: spaces and horizontal tabs.
constexpr bool isOws(char c) noexcept
{
    return c == ' ' || c == '\t';
}

constexpr std::string_view trimOws(std::string_view text) noexcept
{
    while (!text.empty() && isOws(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isOws(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// Calls `visit` with each non-empty, trimmed element of a comma-separated
// list such as "keep-alive, Upgrade".
template <typename Visitor> constexpr void forEachListElement(std::string_view list, Visitor visit)
{
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view element = trimOws(list.substr(0, comma));
        if (!element.empty()) {
            visit(element);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
}

} // namespace http::detail
