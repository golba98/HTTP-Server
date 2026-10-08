#pragma once

#include "http/FileDescriptor.hpp"
#include "http/Message.hpp"

#include <filesystem>
#include <string_view>

namespace http {

// Serves GET and HEAD requests from a directory tree. Lookups are resolved by
// the kernel with openat2(RESOLVE_BENEATH), so neither ".." nor a symlink can
// reach a file outside the root.
class StaticFileHandler {
public:
    // Throws std::system_error if `root` cannot be opened as a directory.
    explicit StaticFileHandler(const std::filesystem::path& root);

    // Safe to call from several threads at once.
    [[nodiscard]] Response handle(const Request& request) const;

private:
    FileDescriptor root_;
};

// The Content-Type for a file, chosen by its extension.
[[nodiscard]] std::string_view contentTypeFor(std::string_view fileName) noexcept;

} // namespace http
