#pragma once

#include "http/FileDescriptor.hpp"
#include "http/Status.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace http {

struct Header {
    std::string name;
    std::string value;

    friend bool operator==(const Header&, const Header&) = default;
};

enum class Version { Http10, Http11 };

struct Request {
    std::string method;
    std::string target;
    Version version{Version::Http11};
    std::vector<Header> headers;
    std::string body;

    // The first field called `name`, compared case-insensitively.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;

    // Whether the client wants the connection kept open after the response:
    // HTTP/1.1 does unless a Connection field lists "close"; HTTP/1.0 only
    // does when one lists "keep-alive".
    [[nodiscard]] bool keepAlive() const;

    friend bool operator==(const Request&, const Request&) = default;
};

// A response body streamed from an open file.
struct FileBody {
    FileDescriptor file;
    std::uint64_t size{0};
};

struct Response {
    Status status{Status::Ok};
    // Content-Length is added by serializeHead() and must not be set here.
    std::vector<Header> headers;
    std::variant<std::string, FileBody> body;

    [[nodiscard]] std::uint64_t contentLength() const noexcept;
};

// The status line and header section, including the blank line that ends it.
[[nodiscard]] std::string serializeHead(const Response& response);

// A short text/plain response such as "404 Not Found".
[[nodiscard]] Response makeErrorResponse(Status status);

// The HTTP date format (IMF-fixdate), e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
[[nodiscard]] std::string httpDate(std::chrono::system_clock::time_point time);

} // namespace http
