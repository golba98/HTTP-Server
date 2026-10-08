// libFuzzer target for parseRequestPath. Whatever the target, an accepted
// path must be a plain relative path that cannot leave the document root.

#include "http/Path.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace {

// libFuzzer treats an abort as a crash and saves the input that caused it.
void check(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view target{reinterpret_cast<const char*>(data), size};

    const auto path = http::parseRequestPath(target);
    if (!path) {
        check(path.error() == http::Status::BadRequest || path.error() == http::Status::NotFound);
        return 0;
    }

    check(path->raw.starts_with('/'));
    std::string_view relative = path->relative;
    check(!relative.contains('\0'));
    if (relative.empty()) {
        return 0;
    }
    // Every segment is a plain, visible name: never empty, ".", ".." or
    // hidden, so the path cannot climb out or start from '/'.
    for (;;) {
        const std::size_t slash = relative.find('/');
        const std::string_view segment = relative.substr(0, slash);
        check(!segment.empty() && !segment.starts_with('.'));
        if (slash == std::string_view::npos) {
            break;
        }
        relative.remove_prefix(slash + 1);
    }
    return 0;
}
