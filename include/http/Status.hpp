#pragma once

#include <string_view>

namespace http {

// The response status codes this server produces.
enum class Status : int {
    Ok = 200,
    MovedPermanently = 301,
    BadRequest = 400,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    RequestTimeout = 408,
    ContentTooLarge = 413,
    UriTooLong = 414,
    RequestHeaderFieldsTooLarge = 431,
    InternalServerError = 500,
    NotImplemented = 501,
    ServiceUnavailable = 503,
    HttpVersionNotSupported = 505,
};

[[nodiscard]] constexpr int statusCode(Status status) noexcept
{
    return static_cast<int>(status);
}

// The standard reason phrase, e.g. "Not Found".
[[nodiscard]] std::string_view reasonPhrase(Status status) noexcept;

} // namespace http
