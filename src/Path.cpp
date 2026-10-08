#include "http/Path.hpp"

#include "Ascii.hpp"

namespace http {

namespace {

constexpr auto npos = std::string_view::npos;

int hexDigitValue(char c) noexcept
{
    return detail::isDigit(c) ? c - '0' : detail::toLower(c) - 'a' + 10;
}

std::expected<std::string, Status> percentDecode(std::string_view text)
{
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            decoded += text[i];
            continue;
        }
        if (i + 2 >= text.size() || !detail::isHexDigit(text[i + 1]) ||
            !detail::isHexDigit(text[i + 2])) {
            return std::unexpected{Status::BadRequest};
        }
        const char byte =
            static_cast<char>(hexDigitValue(text[i + 1]) * 16 + hexDigitValue(text[i + 2]));
        // A NUL would end the path early in every system call.
        if (byte == '\0') {
            return std::unexpected{Status::BadRequest};
        }
        decoded += byte;
        i += 2;
    }
    return decoded;
}

// The path part of the target, without the query: origin-form starts with
// '/', and absolute-form has its "http://authority" prefix removed.
std::expected<std::string_view, Status> pathOf(std::string_view target)
{
    constexpr std::string_view scheme = "http://";
    if (target.size() >= scheme.size() &&
        detail::equalsIgnoreCase(target.substr(0, scheme.size()), scheme)) {
        target.remove_prefix(scheme.size());
        const std::size_t pathStart = target.find_first_of("/?");
        target = pathStart == npos ? std::string_view{} : target.substr(pathStart);
        if (!target.starts_with('/')) {
            // "http://host" and "http://host?q" both mean the root.
            return std::string_view{"/"};
        }
    } else if (!target.starts_with('/')) {
        return std::unexpected{Status::BadRequest};
    }
    return target.substr(0, target.find('?'));
}

} // namespace

std::expected<RequestPath, Status> parseRequestPath(std::string_view target)
{
    const auto path = pathOf(target);
    if (!path) {
        return std::unexpected{path.error()};
    }
    // Clients never send fragments; a raw '#' means a malformed target.
    if (path->contains('#')) {
        return std::unexpected{Status::BadRequest};
    }

    // Decode before splitting so that "%2F" cannot smuggle a separator, and
    // "%2e%2e" cannot hide a ".." segment, past the checks below.
    const auto decoded = percentDecode(*path);
    if (!decoded) {
        return std::unexpected{decoded.error()};
    }

    std::string relative;
    std::string_view rest = *decoded;
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view segment = rest.substr(0, slash);
        rest = slash == npos ? std::string_view{} : rest.substr(slash + 1);

        if (segment.empty() || segment == ".") {
            continue;
        }
        if (segment == "..") {
            return std::unexpected{Status::BadRequest};
        }
        if (segment.starts_with('.')) {
            return std::unexpected{Status::NotFound};
        }
        if (!relative.empty()) {
            relative += '/';
        }
        relative += segment;
    }
    return RequestPath{std::string{*path}, std::move(relative)};
}

} // namespace http
