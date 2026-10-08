#include "http/Message.hpp"

#include "Ascii.hpp"

#include <format>

namespace http {

std::optional<std::string_view> Request::header(std::string_view name) const
{
    for (const Header& field : headers) {
        if (detail::equalsIgnoreCase(field.name, name)) {
            return field.value;
        }
    }
    return std::nullopt;
}

bool Request::keepAlive() const
{
    bool close = false;
    bool keepAlive = false;
    for (const Header& field : headers) {
        if (!detail::equalsIgnoreCase(field.name, "Connection")) {
            continue;
        }
        detail::forEachListElement(field.value, [&](std::string_view option) {
            close = close || detail::equalsIgnoreCase(option, "close");
            keepAlive = keepAlive || detail::equalsIgnoreCase(option, "keep-alive");
        });
    }
    if (close) {
        return false;
    }
    return version == Version::Http11 || keepAlive;
}

std::uint64_t Response::contentLength() const noexcept
{
    if (const auto* text = std::get_if<std::string>(&body)) {
        return text->size();
    }
    if (const auto* file = std::get_if<FileBody>(&body)) {
        return file->size;
    }
    return 0;
}

std::string serializeHead(const Response& response)
{
    std::string head = std::format(
        "HTTP/1.1 {} {}\r\n", statusCode(response.status), reasonPhrase(response.status));
    for (const Header& field : response.headers) {
        head += std::format("{}: {}\r\n", field.name, field.value);
    }
    head += std::format("Content-Length: {}\r\n\r\n", response.contentLength());
    return head;
}

Response makeErrorResponse(Status status)
{
    Response response;
    response.status = status;
    response.headers = {{"Content-Type", "text/plain; charset=utf-8"}};
    response.body = std::format("{} {}\n", statusCode(status), reasonPhrase(status));
    return response;
}

std::string httpDate(std::chrono::system_clock::time_point time)
{
    // Without the L option, std::format uses the C locale, so day and month
    // names are always English as HTTP requires.
    return std::format(
        "{:%a, %d %b %Y %H:%M:%S} GMT", std::chrono::floor<std::chrono::seconds>(time));
}

} // namespace http
