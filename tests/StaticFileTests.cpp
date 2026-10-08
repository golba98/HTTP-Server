#include "http/StaticFiles.hpp"

#include "support/TempDir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

using http::Request;
using http::Response;
using http::StaticFileHandler;
using http::Status;

namespace {

Request makeRequest(std::string target, std::string method = "GET")
{
    Request request;
    request.method = std::move(method);
    request.target = std::move(target);
    request.headers = {{"Host", "localhost"}};
    return request;
}

std::optional<std::string> headerValue(const Response& response, std::string_view name)
{
    for (const auto& field : response.headers) {
        if (field.name == name) {
            return field.value;
        }
    }
    return std::nullopt;
}

// The whole body, read back from the open file for file responses.
std::string bodyOf(const Response& response)
{
    if (const auto* text = std::get_if<std::string>(&response.body)) {
        return *text;
    }
    const auto& file = std::get<http::FileBody>(response.body);
    std::string contents(file.size, '\0');
    const ssize_t got = ::pread(file.file.get(), contents.data(), contents.size(), 0);
    REQUIRE(got == static_cast<ssize_t>(contents.size()));
    return contents;
}

struct Site {
    test::TempDir root;
    test::TempDir outside;

    Site()
    {
        root.write("index.html", "<h1>home</h1>");
        root.write("hello.txt", "hello");
        root.write("sub/index.html", "<p>sub</p>");
        root.write("empty/.keep", "");
        root.write(".secret", "do not serve");
        outside.write("outside.txt", "outside the root");
    }
};

} // namespace

TEST_CASE("StaticFileHandler serves a regular file", "[StaticFiles]")
{
    const Site site;
    const StaticFileHandler handler{site.root.path()};

    for (const char* method : {"GET", "HEAD"}) {
        INFO("method: " << method);
        const Response response = handler.handle(makeRequest("/hello.txt", method));

        CHECK(response.status == Status::Ok);
        CHECK(headerValue(response, "Content-Type") == "text/plain; charset=utf-8");
        CHECK(response.contentLength() == 5);
        CHECK(bodyOf(response) == "hello");
    }
}

TEST_CASE("StaticFileHandler serves index.html for directories", "[StaticFiles]")
{
    const Site site;
    const StaticFileHandler handler{site.root.path()};

    SECTION("at the root")
    {
        const Response response = handler.handle(makeRequest("/"));
        CHECK(response.status == Status::Ok);
        CHECK(headerValue(response, "Content-Type") == "text/html; charset=utf-8");
        CHECK(bodyOf(response) == "<h1>home</h1>");
    }

    SECTION("in a subdirectory")
    {
        const Response response = handler.handle(makeRequest("/sub/"));
        CHECK(response.status == Status::Ok);
        CHECK(bodyOf(response) == "<p>sub</p>");
    }

    SECTION("redirecting when the trailing slash is missing")
    {
        const Response response = handler.handle(makeRequest("/sub"));
        CHECK(response.status == Status::MovedPermanently);
        CHECK(headerValue(response, "Location") == "/sub/");
    }

    SECTION("answering 404 when there is no index.html")
    {
        CHECK(handler.handle(makeRequest("/empty/")).status == Status::NotFound);
    }
}

TEST_CASE("StaticFileHandler answers 404 for missing and hidden files", "[StaticFiles]")
{
    const Site site;
    const StaticFileHandler handler{site.root.path()};

    CHECK(handler.handle(makeRequest("/missing.txt")).status == Status::NotFound);
    CHECK(handler.handle(makeRequest("/hello.txt/x")).status == Status::NotFound);
    CHECK(handler.handle(makeRequest("/.secret")).status == Status::NotFound);
}

TEST_CASE("StaticFileHandler rejects traversal in the target", "[StaticFiles]")
{
    const Site site;
    const StaticFileHandler handler{site.root.path()};

    CHECK(handler.handle(makeRequest("/../outside.txt")).status == Status::BadRequest);
    CHECK(handler.handle(makeRequest("/%2e%2e/outside.txt")).status == Status::BadRequest);
}

TEST_CASE("StaticFileHandler follows symlinks only within the root", "[StaticFiles]")
{
    const Site site;
    const auto& root = site.root.path();
    std::filesystem::create_symlink("hello.txt", root / "inside-link.txt");
    std::filesystem::create_symlink(
        site.outside.path() / "outside.txt", root / "absolute-escape.txt");
    std::filesystem::create_symlink(
        std::filesystem::relative(site.outside.path() / "outside.txt", root),
        root / "relative-escape.txt");
    const StaticFileHandler handler{root};

    CHECK(bodyOf(handler.handle(makeRequest("/inside-link.txt"))) == "hello");
    CHECK(handler.handle(makeRequest("/absolute-escape.txt")).status == Status::Forbidden);
    CHECK(handler.handle(makeRequest("/relative-escape.txt")).status == Status::Forbidden);
}

TEST_CASE("StaticFileHandler refuses files that are not regular", "[StaticFiles]")
{
    const Site site;
    // Opening a FIFO for reading would block forever without O_NONBLOCK.
    REQUIRE(::mkfifo((site.root.path() / "pipe").c_str(), 0600) == 0);
    const StaticFileHandler handler{site.root.path()};

    CHECK(handler.handle(makeRequest("/pipe")).status == Status::Forbidden);
}

TEST_CASE("StaticFileHandler answers 403 for unreadable files", "[StaticFiles]")
{
    if (::geteuid() == 0) {
        SKIP("root can read files regardless of permissions");
    }
    const Site site;
    std::filesystem::permissions(site.root.path() / "hello.txt", std::filesystem::perms::none);
    const StaticFileHandler handler{site.root.path()};

    CHECK(handler.handle(makeRequest("/hello.txt")).status == Status::Forbidden);
}

TEST_CASE("StaticFileHandler methods", "[StaticFiles]")
{
    const Site site;
    const StaticFileHandler handler{site.root.path()};

    SECTION("other standard methods are not allowed")
    {
        for (const char* method : {"POST", "PUT", "DELETE", "PATCH", "OPTIONS"}) {
            INFO("method: " << method);
            const Response response = handler.handle(makeRequest("/hello.txt", method));
            CHECK(response.status == Status::MethodNotAllowed);
            CHECK(headerValue(response, "Allow") == "GET, HEAD");
        }
    }

    SECTION("unknown methods are not implemented")
    {
        CHECK(handler.handle(makeRequest("/hello.txt", "BREW")).status == Status::NotImplemented);
    }
}

TEST_CASE("StaticFileHandler requires a directory as its root", "[StaticFiles]")
{
    const Site site;

    CHECK_THROWS_AS(StaticFileHandler{site.root.path() / "hello.txt"}, std::system_error);
    CHECK_THROWS_AS(StaticFileHandler{site.root.path() / "missing"}, std::system_error);
}

TEST_CASE("contentTypeFor", "[StaticFiles]")
{
    CHECK(http::contentTypeFor("index.html") == "text/html; charset=utf-8");
    CHECK(http::contentTypeFor("style.CSS") == "text/css; charset=utf-8");
    CHECK(http::contentTypeFor("app.mjs") == "text/javascript; charset=utf-8");
    CHECK(http::contentTypeFor("logo.PNG") == "image/png");
    CHECK(http::contentTypeFor("photo.jpeg") == "image/jpeg");
    CHECK(http::contentTypeFor("archive.tar.gz") == "application/octet-stream");
    CHECK(http::contentTypeFor("README") == "application/octet-stream");
    CHECK(http::contentTypeFor("dir.d/README") == "application/octet-stream");
}
