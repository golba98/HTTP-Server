#include "http/Status.hpp"

namespace http {

std::string_view reasonPhrase(Status status) noexcept
{
    switch (status) {
    case Status::Ok:
        return "OK";
    case Status::MovedPermanently:
        return "Moved Permanently";
    case Status::BadRequest:
        return "Bad Request";
    case Status::Forbidden:
        return "Forbidden";
    case Status::NotFound:
        return "Not Found";
    case Status::MethodNotAllowed:
        return "Method Not Allowed";
    case Status::RequestTimeout:
        return "Request Timeout";
    case Status::ContentTooLarge:
        return "Content Too Large";
    case Status::UriTooLong:
        return "URI Too Long";
    case Status::RequestHeaderFieldsTooLarge:
        return "Request Header Fields Too Large";
    case Status::InternalServerError:
        return "Internal Server Error";
    case Status::NotImplemented:
        return "Not Implemented";
    case Status::ServiceUnavailable:
        return "Service Unavailable";
    case Status::HttpVersionNotSupported:
        return "HTTP Version Not Supported";
    }
    return "Unknown";
}

} // namespace http
