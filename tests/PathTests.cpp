#include "http/Path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <string_view>

using http::Status;
using namespace std::string_view_literals;

namespace {

std::string relativePath(std::string_view target)
{
    INFO("target: " << target);
    const auto path = http::parseRequestPath(target);
    REQUIRE(path.has_value());
    return path->relative;
}

Status pathError(std::string_view target)
{
    INFO("target: " << target);
    const auto path = http::parseRequestPath(target);
    REQUIRE_FALSE(path.has_value());
    return path.error();
}

} // namespace

TEST_CASE("parseRequestPath maps a target below the document root", "[Path]")
{
    CHECK(relativePath("/") == "");
    CHECK(relativePath("/index.html") == "index.html");
    CHECK(relativePath("/a/b/c.txt") == "a/b/c.txt");
}

TEST_CASE("parseRequestPath drops empty and '.' segments", "[Path]")
{
    CHECK(relativePath("//a///b/") == "a/b");
    CHECK(relativePath("/./a/./b/.") == "a/b");
}

TEST_CASE("parseRequestPath decodes percent-escapes", "[Path]")
{
    CHECK(relativePath("/a%20b.txt") == "a b.txt");
    CHECK(relativePath("/caf%C3%A9") == "caf\xc3\xa9");
    CHECK(relativePath("/caf%c3%a9") == "caf\xc3\xa9");
    CHECK(relativePath("/100%25") == "100%");
}

TEST_CASE("parseRequestPath ignores the query", "[Path]")
{
    CHECK(relativePath("/a?x=1/../../etc") == "a");
    CHECK(relativePath("/?") == "");
}

TEST_CASE("parseRequestPath keeps the raw path for redirects", "[Path]")
{
    const auto path = http::parseRequestPath("/docs%20x/?q=1");
    REQUIRE(path.has_value());
    CHECK(path->raw == "/docs%20x/");
    CHECK(path->relative == "docs x");
}

TEST_CASE("parseRequestPath accepts absolute-form targets", "[Path]")
{
    const auto path = http::parseRequestPath("http://example.com/a/b?x");
    REQUIRE(path.has_value());
    CHECK(path->raw == "/a/b");
    CHECK(path->relative == "a/b");

    CHECK(relativePath("HTTP://example.com") == "");
    CHECK(relativePath("http://example.com:8080?x") == "");
}

TEST_CASE("parseRequestPath rejects any attempt to climb out of the root", "[Path]")
{
    for (const std::string_view target : {
             "/..",
             "/../etc/passwd",
             "/a/../b",
             "/a/..",
             "/%2e%2e/etc/passwd",
             "/%2E%2E/etc/passwd",
             "/.%2e/etc/passwd",
             "/a/..%2F..%2Fetc",
             "/a%2f..%2f..%2fetc",
             "http://example.com/../etc",
         }) {
        CHECK(pathError(target) == Status::BadRequest);
    }
}

TEST_CASE("parseRequestPath rejects malformed targets", "[Path]")
{
    for (const std::string_view target : std::initializer_list<std::string_view>{
             "",
             "*",
             "a/b",
             "example.com:443",
             "/%",
             "/%2",
             "/%zz",
             "/%2g",
             "/a%00b",
             "/a#fragment",
             "ftp://example.com/a",
         }) {
        CHECK(pathError(target) == Status::BadRequest);
    }
}

TEST_CASE("parseRequestPath hides dot-files", "[Path]")
{
    CHECK(pathError("/.git/config") == Status::NotFound);
    CHECK(pathError("/a/.env") == Status::NotFound);
    CHECK(pathError("/%2egit") == Status::NotFound);
    CHECK(pathError("/...") == Status::NotFound);
}
