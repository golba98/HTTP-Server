#include "http/RequestParser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using http::Header;
using http::ParsedRequest;
using http::ParseResult;
using http::ParserLimits;
using http::RequestParser;
using http::Status;
using http::Version;
using namespace std::string_view_literals;

namespace {

ParseResult parseAtOnce(std::string_view input, ParserLimits limits = {})
{
    RequestParser parser{limits};
    return parser.parse(input);
}

// Feeds `input` one byte at a time, as the slowest possible client would, and
// returns the first result that is not "need more data".
ParseResult parseByteByByte(std::string_view input, ParserLimits limits = {})
{
    RequestParser parser{limits};
    for (std::size_t length = 1; length <= input.size(); ++length) {
        auto result = parser.parse(input.substr(0, length));
        if (!result || result->has_value()) {
            return result;
        }
    }
    return std::optional<ParsedRequest>{};
}

// Parses `input` both ways, checks both agree, and returns the request.
ParsedRequest parseComplete(std::string_view input, ParserLimits limits = {})
{
    INFO("input: " << input);
    auto atOnce = parseAtOnce(input, limits);
    if (!atOnce) {
        FAIL("parse error: " << atOnce.error().message);
    }
    REQUIRE(atOnce->has_value());

    const auto byteByByte = parseByteByByte(input, limits);
    REQUIRE(byteByByte.has_value());
    REQUIRE(byteByByte->has_value());
    CHECK((*byteByByte)->request == (*atOnce)->request);
    CHECK((*byteByByte)->consumed == (*atOnce)->consumed);

    return std::move(**atOnce);
}

// Parses `input` both ways and returns the error status, which must agree.
Status parseError(std::string_view input, ParserLimits limits = {})
{
    INFO("input: " << input);
    const auto atOnce = parseAtOnce(input, limits);
    REQUIRE_FALSE(atOnce.has_value());

    const auto byteByByte = parseByteByByte(input, limits);
    REQUIRE_FALSE(byteByByte.has_value());
    CHECK(byteByByte.error().status == atOnce.error().status);

    return atOnce.error().status;
}

} // namespace

TEST_CASE("RequestParser parses a simple GET", "[RequestParser]")
{
    const std::string input = "GET /index.html?q=1 HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "Accept:  text/html \r\n"
                              "X-Empty:\r\n"
                              "\r\n";

    const ParsedRequest parsed = parseComplete(input);

    CHECK(parsed.consumed == input.size());
    CHECK(parsed.request.method == "GET");
    CHECK(parsed.request.target == "/index.html?q=1");
    CHECK(parsed.request.version == Version::Http11);
    CHECK(parsed.request.headers ==
        std::vector<Header>{{"Host", "example.com"}, {"Accept", "text/html"}, {"X-Empty", ""}});
    CHECK(parsed.request.body.empty());
}

TEST_CASE("RequestParser waits for the rest of an incomplete request", "[RequestParser]")
{
    const std::string input = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";

    for (std::size_t length = 0; length < input.size(); ++length) {
        INFO("length: " << length);
        const auto result = parseAtOnce(input.substr(0, length));
        REQUIRE(result.has_value());
        CHECK_FALSE(result->has_value());
    }
}

TEST_CASE("RequestParser leaves pipelined requests for the next call", "[RequestParser]")
{
    const std::string first = "GET /one HTTP/1.1\r\nHost: a\r\n\r\n";
    const std::string second = "GET /two HTTP/1.1\r\nHost: a\r\n\r\n";
    const std::string buffer = first + second;
    RequestParser parser;

    const auto one = parser.parse(buffer);
    REQUIRE(one.has_value());
    REQUIRE(one->has_value());
    CHECK((*one)->request.target == "/one");
    CHECK((*one)->consumed == first.size());

    const auto two = parser.parse(std::string_view{buffer}.substr((*one)->consumed));
    REQUIRE(two.has_value());
    REQUIRE(two->has_value());
    CHECK((*two)->request.target == "/two");
    CHECK((*two)->consumed == second.size());
}

TEST_CASE("RequestParser skips empty lines before the request line", "[RequestParser]")
{
    const std::string input = "\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n";

    const ParsedRequest parsed = parseComplete(input);

    CHECK(parsed.request.method == "GET");
    CHECK(parsed.consumed == input.size());
}

TEST_CASE("RequestParser request line", "[RequestParser]")
{
    SECTION("accepts HTTP/1.0 without a Host field")
    {
        CHECK(parseComplete("GET / HTTP/1.0\r\n\r\n").request.version == Version::Http10);
    }

    SECTION("treats a later HTTP/1.x minor version as HTTP/1.1")
    {
        CHECK(
            parseComplete("GET / HTTP/1.2\r\nHost: a\r\n\r\n").request.version == Version::Http11);
    }

    SECTION("accepts any token as a method")
    {
        CHECK(parseComplete("M-SEARCH * HTTP/1.1\r\nHost: a\r\n\r\n").request.method == "M-SEARCH");
    }

    SECTION("rejects malformed lines")
    {
        for (const std::string_view line : {
                 "GET /\r\n",
                 "GET  / HTTP/1.1\r\n",
                 "GET / HTTP/1.1 \r\n",
                 "GET /a b HTTP/1.1\r\n",
                 "G(T / HTTP/1.1\r\n",
                 " GET / HTTP/1.1\r\n",
                 "GET /\x01 HTTP/1.1\r\n",
                 "GET /\x7f HTTP/1.1\r\n",
                 "GET /caf\xc3\xa9 HTTP/1.1\r\n",
                 "GET / http/1.1\r\n",
                 "GET / HTTP/1.1x\r\n",
                 "GET / HTTP/11\r\n",
                 "GET / HTTP/1.\r\n",
                 "GET / HTTP/1.1\n",
                 "GET / HTTP/1.1\r\r\n",
             }) {
            CHECK(parseError(std::string{line} + "Host: a\r\n\r\n") == Status::BadRequest);
        }
    }

    SECTION("rejects HTTP major versions other than 1")
    {
        CHECK(parseError("GET / HTTP/2.0\r\nHost: a\r\n\r\n") == Status::HttpVersionNotSupported);
        CHECK(parseError("GET / HTTP/0.9\r\n\r\n") == Status::HttpVersionNotSupported);
    }

    SECTION("rejects a line over the limit with 414")
    {
        const ParserLimits limits{.maxRequestLineBytes = 32};
        const std::string fits = "GET /" + std::string(16, 'a') + " HTTP/1.1\r\n";
        REQUIRE(fits.size() == 32);

        CHECK(parseComplete(fits + "Host: a\r\n\r\n", limits).request.target.size() == 17);
        CHECK(parseError("GET /" + std::string(17, 'a') + " HTTP/1.1\r\nHost: a\r\n\r\n", limits) ==
            Status::UriTooLong);
        // Also when the line has not finished arriving.
        CHECK(parseError("GET /" + std::string(100, 'a'), limits) == Status::UriTooLong);
    }
}

TEST_CASE("RequestParser header fields", "[RequestParser]")
{
    SECTION("rejects malformed fields")
    {
        for (const std::string_view field : std::initializer_list<std::string_view>{
                 "NoColon\r\n",
                 "Name : value\r\n",
                 ": value\r\n",
                 "Bad(Name): value\r\n",
                 " Leading: space\r\n",
                 "X-Nul: a\0b\r\n"sv,
                 "X-Cr: a\rb\r\n",
                 "X-Del: a\x7f\r\n",
                 "X-Bare-Lf: a\n",
             }) {
            CHECK(parseError("GET / HTTP/1.1\r\nHost: a\r\n" + std::string{field} + "\r\n") ==
                Status::BadRequest);
        }
    }

    SECTION("rejects obsolete line folding")
    {
        CHECK(parseError("GET / HTTP/1.1\r\nHost: a\r\nX-Folded: one\r\n two\r\n\r\n") ==
            Status::BadRequest);
    }

    SECTION("accepts obs-text and tabs in values")
    {
        const auto parsed =
            parseComplete("GET / HTTP/1.1\r\nHost: a\r\nX: \tcaf\xc3\xa9\tb\r\n\r\n");
        CHECK(parsed.request.header("X") == "caf\xc3\xa9\tb");
    }

    SECTION("requires exactly one Host field in HTTP/1.1")
    {
        CHECK(parseError("GET / HTTP/1.1\r\n\r\n") == Status::BadRequest);
        CHECK(parseError("GET / HTTP/1.1\r\nHost: a\r\nhost: b\r\n\r\n") == Status::BadRequest);
        CHECK(parseError("GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n") == Status::BadRequest);
    }

    SECTION("rejects a header section over the byte limit with 431")
    {
        const ParserLimits limits{.maxHeaderBytes = 32};
        // 30 bytes of fields plus the final CRLF would be 32.
        const std::string fits = "Host: a\r\nX: " + std::string(16, 'v') + "\r\n";
        REQUIRE(fits.size() + 2 == 32);

        CHECK(parseComplete("GET / HTTP/1.1\r\n" + fits + "\r\n", limits).request.headers.size() ==
            2);
        CHECK(parseError("GET / HTTP/1.1\r\nHost: a\r\nX: " + std::string(17, 'v') + "\r\n\r\n",
                  limits) == Status::RequestHeaderFieldsTooLarge);
        CHECK(parseError("GET / HTTP/1.1\r\nX: " + std::string(100, 'v'), limits) ==
            Status::RequestHeaderFieldsTooLarge);
    }

    SECTION("rejects more fields than the count limit with 431")
    {
        const ParserLimits limits{.maxHeaderCount = 3};

        CHECK(parseComplete("GET / HTTP/1.1\r\nHost: a\r\nA: 1\r\nB: 2\r\n\r\n", limits)
                  .request.headers.size() == 3);
        CHECK(parseError("GET / HTTP/1.1\r\nHost: a\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", limits) ==
            Status::RequestHeaderFieldsTooLarge);
    }
}

TEST_CASE("RequestParser Content-Length bodies", "[RequestParser]")
{
    SECTION("reads exactly Content-Length bytes")
    {
        const std::string input =
            "POST /submit HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";

        const auto parsed = parseComplete(input + "GET / HTTP/1.1\r\n");

        CHECK(parsed.request.body == "hello");
        CHECK(parsed.consumed == input.size());
    }

    SECTION("accepts a zero length")
    {
        CHECK(parseComplete("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n")
                .request.body.empty());
    }

    SECTION("waits for the whole body")
    {
        const auto result =
            parseAtOnce("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nhello");
        REQUIRE(result.has_value());
        CHECK_FALSE(result->has_value());
    }

    SECTION("rejects invalid lengths")
    {
        for (const std::string_view length : {
                 "",
                 "-1",
                 "+5",
                 "5 5",
                 "0x10",
                 "5, 5",
                 "99999999999999999999999",
             }) {
            CHECK(parseError("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: " +
                      std::string{length} + "\r\n\r\n") == Status::BadRequest);
        }
    }

    SECTION("rejects repeated Content-Length fields")
    {
        CHECK(
            parseError(
                "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx") ==
            Status::BadRequest);
    }

    SECTION("rejects a body over the limit with 413")
    {
        const ParserLimits limits{.maxBodyBytes = 4};

        CHECK(parseComplete("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n\r\nabcd", limits)
                  .request.body == "abcd");
        CHECK(parseError("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n", limits) ==
            Status::ContentTooLarge);
    }
}

TEST_CASE("RequestParser chunked bodies", "[RequestParser]")
{
    const std::string head = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n";

    SECTION("decodes chunks up to the last chunk")
    {
        const std::string input = head + "5\r\nhello\r\n7\r\n, world\r\n0\r\n\r\n";

        const auto parsed = parseComplete(input + "next");

        CHECK(parsed.request.body == "hello, world");
        CHECK(parsed.consumed == input.size());
    }

    SECTION("accepts upper- and lower-case hex sizes")
    {
        const std::string data(26, 'x');
        CHECK(parseComplete(head + "1A\r\n" + data + "\r\n1a\r\n" + data + "\r\n0\r\n\r\n")
                  .request.body.size() == 52);
    }

    SECTION("ignores chunk extensions")
    {
        CHECK(
            parseComplete(head + "3;name=value\r\nabc\r\n0 ; last\r\n\r\n").request.body == "abc");
    }

    SECTION("discards trailer fields")
    {
        const auto parsed = parseComplete(head + "3\r\nabc\r\n0\r\nX-Checksum: 123\r\n\r\n");

        CHECK(parsed.request.body == "abc");
        CHECK_FALSE(parsed.request.header("X-Checksum").has_value());
    }

    SECTION("treats the coding name case-insensitively")
    {
        CHECK(parseComplete(
            "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: Chunked\r\n\r\n0\r\n\r\n")
                .request.body.empty());
    }

    SECTION("rejects malformed chunks")
    {
        for (const std::string_view chunks : {
                 "zz\r\n",
                 "\r\n",
                 "-1\r\n",
                 "3\r\nabcX\r\n0\r\n\r\n",
                 // Would decode cleanly if the two bytes after the data were
                 // skipped without being checked.
                 "3\r\nabcXY0\r\n\r\n",
                 "3\r\nabc\r\n0\r\nBad Trailer\r\n\r\n",
                 "3\nabc\r\n0\r\n\r\n",
                 "3 x\r\nabc\r\n0\r\n\r\n",
                 "11111111111111111\r\n",
             }) {
            CHECK(parseError(head + std::string{chunks}) == Status::BadRequest);
        }
    }

    SECTION("rejects a decoded body over the limit with 413")
    {
        const ParserLimits limits{.maxBodyBytes = 5};

        CHECK(parseComplete(head + "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", limits).request.body ==
            "abcde");
        CHECK(parseError(head + "3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n", limits) ==
            Status::ContentTooLarge);
        CHECK(parseError(head + "ffffffffffffffff\r\n", limits) == Status::ContentTooLarge);
    }
}

TEST_CASE("RequestParser message framing", "[RequestParser]")
{
    SECTION("rejects Transfer-Encoding together with Content-Length")
    {
        CHECK(parseError("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
                         "Content-Length: 3\r\n\r\n0\r\n\r\n") == Status::BadRequest);
    }

    SECTION("rejects Transfer-Encoding in HTTP/1.0")
    {
        CHECK(parseError("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n") ==
            Status::BadRequest);
    }

    SECTION("rejects a coding list that does not end in chunked")
    {
        CHECK(parseError("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n") ==
            Status::BadRequest);
        CHECK(
            parseError("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked, gzip\r\n\r\n") ==
            Status::BadRequest);
        CHECK(parseError("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding:\r\n\r\n") ==
            Status::BadRequest);
    }

    SECTION("answers other codings before chunked with 501")
    {
        CHECK(
            parseError("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n") ==
            Status::NotImplemented);
        CHECK(parseError("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n") == Status::NotImplemented);
    }
}

TEST_CASE("RequestParser is ready for a new message after an error", "[RequestParser]")
{
    RequestParser parser;

    REQUIRE_FALSE(parser.parse("BAD\r\n\r\n").has_value());

    const auto result = parser.parse("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
    REQUIRE(result.has_value());
    CHECK(result->has_value());
}
