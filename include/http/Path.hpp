#pragma once

#include "http/Status.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace http {

struct RequestPath {
    // The path as sent, without the query, e.g. "/docs/a%20b.txt". Redirects
    // are built from it.
    std::string raw;
    // The decoded path relative to the document root, e.g. "docs/a b.txt";
    // "" is the root itself. It contains no "..", "." or empty segments and
    // does not start with '/', so it can never name anything above the root.
    std::string relative;
};

// Extracts the path from an origin-form ("/a/b?q") or absolute-form
// ("http://host/a/b?q") request-target. Fails with BadRequest for a malformed
// target or any ".." segment, which well-behaved clients never send, and with
// NotFound for hidden (dot-file) segments such as ".git".
[[nodiscard]] std::expected<RequestPath, Status> parseRequestPath(std::string_view target);

} // namespace http
