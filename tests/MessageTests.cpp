#include "http/Message.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include <sys/mman.h>

using http::Header;
using http::Request;
using http::Response;
using http::Status;
using http::Version;

namespace {

Request makeRequest(Version version, std::vector<Header> headers)
{
    Request request;
    request.method = "GET";
    request.target = "/";
    request.version = version;
    request.headers = std::move(headers);
    return request;
}

} // namespace

TEST_CASE("Request header lookup", "[Message]")
{
    const Request request = makeRequest(Version::Http11,
        {{"Host", "example.com"}, {"Accept", "text/html"}, {"accept", "text/plain"}});

    SECTION("ignores the case of the name")
    {
        CHECK(request.header("HOST") == "example.com");
        CHECK(request.header("host") == "example.com");
    }

    SECTION("returns the first of repeated fields")
    {
        CHECK(request.header("Accept") == "text/html");
    }

    SECTION("returns nothing for a missing field")
    {
        CHECK_FALSE(request.header("Cookie").has_value());
    }
}

TEST_CASE("Request keepAlive", "[Message]")
{
    SECTION("HTTP/1.1 keeps the connection open by default")
    {
        CHECK(makeRequest(Version::Http11, {}).keepAlive());
    }

    SECTION("HTTP/1.1 closes when asked to")
    {
        CHECK_FALSE(makeRequest(Version::Http11, {{"Connection", "close"}}).keepAlive());
        CHECK_FALSE(
            makeRequest(Version::Http11, {{"connection", "Keep-Alive, CLOSE"}}).keepAlive());
        CHECK_FALSE(
            makeRequest(Version::Http11, {{"Connection", "upgrade"}, {"Connection", "close"}})
                .keepAlive());
    }

    SECTION("HTTP/1.0 closes by default")
    {
        CHECK_FALSE(makeRequest(Version::Http10, {}).keepAlive());
    }

    SECTION("HTTP/1.0 keeps the connection open when asked to")
    {
        CHECK(makeRequest(Version::Http10, {{"Connection", "Keep-Alive"}}).keepAlive());
    }

    SECTION("a token that merely contains 'close' does not count")
    {
        CHECK(makeRequest(Version::Http11, {{"Connection", "closed"}}).keepAlive());
    }
}

TEST_CASE("reasonPhrase", "[Message]")
{
    CHECK(http::reasonPhrase(Status::Ok) == "OK");
    CHECK(http::reasonPhrase(Status::NotFound) == "Not Found");
    CHECK(http::reasonPhrase(Status::RequestHeaderFieldsTooLarge) ==
        "Request Header Fields Too Large");
    CHECK(http::reasonPhrase(Status::HttpVersionNotSupported) == "HTTP Version Not Supported");
}

TEST_CASE("serializeHead", "[Message]")
{
    SECTION("writes the status line, the headers, and Content-Length of a text body")
    {
        Response response;
        response.status = Status::NotFound;
        response.headers = {{"Content-Type", "text/plain"}, {"X-Test", "1"}};
        response.body = std::string{"missing"};

        CHECK(http::serializeHead(response) ==
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "X-Test: 1\r\n"
            "Content-Length: 7\r\n"
            "\r\n");
    }

    SECTION("uses the size of a file body")
    {
        Response response;
        response.body = http::FileBody{
            http::FileDescriptor{::memfd_create("serializeHead", MFD_CLOEXEC)}, 123456};

        CHECK(http::serializeHead(response) ==
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 123456\r\n"
            "\r\n");
    }
}

TEST_CASE("makeErrorResponse", "[Message]")
{
    const Response response = http::makeErrorResponse(Status::NotFound);

    CHECK(response.status == Status::NotFound);
    CHECK(std::get<std::string>(response.body) == "404 Not Found\n");
    REQUIRE(response.headers.size() == 1);
    CHECK(response.headers[0] == Header{"Content-Type", "text/plain; charset=utf-8"});
}

TEST_CASE("httpDate formats an IMF-fixdate in GMT", "[Message]")
{
    // The example date from RFC 9110, section 5.6.7.
    const std::chrono::system_clock::time_point time{std::chrono::seconds{784111777}};

    CHECK(http::httpDate(time) == "Sun, 06 Nov 1994 08:49:37 GMT");
    CHECK(http::httpDate(time + std::chrono::milliseconds{999}) == "Sun, 06 Nov 1994 08:49:37 GMT");
}
