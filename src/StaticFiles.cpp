#include "http/StaticFiles.hpp"

#include "Ascii.hpp"
#include "SystemError.hpp"
#include "http/Path.hpp"

#include <array>
#include <cerrno>
#include <expected>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace http {

namespace {

struct ContentType {
    std::string_view extension;
    std::string_view type;
};

constexpr std::array contentTypes{
    ContentType{"css", "text/css; charset=utf-8"},
    ContentType{"gif", "image/gif"},
    ContentType{"htm", "text/html; charset=utf-8"},
    ContentType{"html", "text/html; charset=utf-8"},
    ContentType{"ico", "image/vnd.microsoft.icon"},
    ContentType{"jpeg", "image/jpeg"},
    ContentType{"jpg", "image/jpeg"},
    ContentType{"js", "text/javascript; charset=utf-8"},
    ContentType{"json", "application/json"},
    ContentType{"mjs", "text/javascript; charset=utf-8"},
    ContentType{"mp3", "audio/mpeg"},
    ContentType{"mp4", "video/mp4"},
    ContentType{"pdf", "application/pdf"},
    ContentType{"png", "image/png"},
    ContentType{"svg", "image/svg+xml"},
    ContentType{"txt", "text/plain; charset=utf-8"},
    ContentType{"wasm", "application/wasm"},
    ContentType{"webm", "video/webm"},
    ContentType{"webp", "image/webp"},
    ContentType{"woff", "font/woff"},
    ContentType{"woff2", "font/woff2"},
    ContentType{"xml", "application/xml"},
};

// Methods defined by RFC 9110 and RFC 5789 that this handler does not allow.
// Anything else is unknown, which earns 501 instead of 405.
constexpr std::array otherStandardMethods{
    std::string_view{"POST"},
    std::string_view{"PUT"},
    std::string_view{"DELETE"},
    std::string_view{"CONNECT"},
    std::string_view{"OPTIONS"},
    std::string_view{"TRACE"},
    std::string_view{"PATCH"},
};

// Opens `path` relative to `directory`, refusing to leave it.
std::expected<FileDescriptor, std::error_code> openBeneath(int directory, const std::string& path)
{
    open_how how{};
    // O_NONBLOCK keeps a FIFO from blocking the open; it has no effect on the
    // regular files that are actually served.
    how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY;
    // RESOLVE_BENEATH fails with EXDEV if ".." or a symlink would leave the
    // directory; RESOLVE_NO_MAGICLINKS blocks /proc/self/fd-style links.
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;

    // EAGAIN means a concurrent rename raced the lookup; retrying is safe.
    constexpr int attempts = 4;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        // glibc has no openat2() wrapper, so the system call is made directly.
        const long fd = ::syscall(SYS_openat2, directory, path.c_str(), &how, sizeof how);
        if (fd >= 0) {
            return FileDescriptor{static_cast<int>(fd)};
        }
        if (errno != EINTR && errno != EAGAIN) {
            break;
        }
    }
    return std::unexpected{detail::lastError()};
}

Status statusForOpenError(std::error_code error) noexcept
{
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory ||
        error == std::errc::too_many_symbolic_link_levels ||
        error == std::errc::filename_too_long) {
        return Status::NotFound;
    }
    // EXDEV is RESOLVE_BENEATH reporting an escape attempt.
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
        error == std::errc::cross_device_link) {
        return Status::Forbidden;
    }
    return Status::InternalServerError;
}

Response methodNotAllowed()
{
    Response response = makeErrorResponse(Status::MethodNotAllowed);
    response.headers.push_back({"Allow", "GET, HEAD"});
    return response;
}

Response redirect(std::string location)
{
    Response response = makeErrorResponse(Status::MovedPermanently);
    response.headers.push_back({"Location", std::move(location)});
    return response;
}

} // namespace

StaticFileHandler::StaticFileHandler(const std::filesystem::path& root)
    : root_{::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)}
{
    if (!root_) {
        detail::throwSystemError("open document root");
    }
}

Response StaticFileHandler::handle(const Request& request) const
{
    if (request.method != "GET" && request.method != "HEAD") {
        for (const std::string_view method : otherStandardMethods) {
            if (request.method == method) {
                return methodNotAllowed();
            }
        }
        return makeErrorResponse(Status::NotImplemented);
    }

    const auto path = parseRequestPath(request.target);
    if (!path) {
        return makeErrorResponse(path.error());
    }

    std::string fileName = path->relative.empty() ? "." : path->relative;
    auto file = openBeneath(root_.get(), fileName);
    if (!file) {
        return makeErrorResponse(statusForOpenError(file.error()));
    }
    struct stat info{};
    if (::fstat(file->get(), &info) != 0) {
        return makeErrorResponse(Status::InternalServerError);
    }

    if (S_ISDIR(info.st_mode)) {
        // Without the trailing slash, relative links in the index page would
        // resolve against the parent directory.
        if (!path->raw.ends_with('/')) {
            return redirect(path->raw + "/");
        }
        fileName = path->relative.empty() ? "index.html" : path->relative + "/index.html";
        file = openBeneath(root_.get(), fileName);
        if (!file) {
            return makeErrorResponse(statusForOpenError(file.error()));
        }
        if (::fstat(file->get(), &info) != 0) {
            return makeErrorResponse(Status::InternalServerError);
        }
    }

    if (!S_ISREG(info.st_mode)) {
        return makeErrorResponse(Status::Forbidden);
    }

    Response response;
    response.headers = {{"Content-Type", std::string{contentTypeFor(fileName)}}};
    response.body = FileBody{std::move(*file), static_cast<std::uint64_t>(info.st_size)};
    return response;
}

std::string_view contentTypeFor(std::string_view fileName) noexcept
{
    const std::size_t slash = fileName.rfind('/');
    const std::string_view baseName =
        slash == std::string_view::npos ? fileName : fileName.substr(slash + 1);
    const std::size_t dot = baseName.rfind('.');
    if (dot != std::string_view::npos) {
        const std::string_view extension = baseName.substr(dot + 1);
        for (const ContentType& entry : contentTypes) {
            if (detail::equalsIgnoreCase(extension, entry.extension)) {
                return entry.type;
            }
        }
    }
    return "application/octet-stream";
}

} // namespace http
