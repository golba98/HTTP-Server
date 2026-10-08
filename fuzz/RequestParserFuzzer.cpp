// libFuzzer target for RequestParser. Besides looking for crashes, it checks
// that parsing an input all at once and one byte at a time agree, and that
// the parser always makes progress through pipelined requests.

#include "http/RequestParser.hpp"

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

bool sameResult(const http::ParseResult& a, const http::ParseResult& b)
{
    if (a.has_value() != b.has_value()) {
        return false;
    }
    if (!a) {
        return a.error().status == b.error().status;
    }
    if (a->has_value() != b->has_value()) {
        return false;
    }
    return !a->has_value() || ((*a)->request == (*b)->request && (*a)->consumed == (*b)->consumed);
}

bool isParseErrorStatus(http::Status status)
{
    using enum http::Status;
    return status == BadRequest || status == ContentTooLarge || status == UriTooLong ||
        status == RequestHeaderFieldsTooLarge || status == NotImplemented ||
        status == HttpVersionNotSupported;
}

// Small limits let short inputs reach every limit check.
constexpr http::ParserLimits limits{
    .maxRequestLineBytes = 64,
    .maxHeaderBytes = 128,
    .maxHeaderCount = 8,
    .maxBodyBytes = 64,
};

http::ParseResult parseByteByByte(std::string_view input)
{
    http::RequestParser parser{limits};
    http::ParseResult result = std::optional<http::ParsedRequest>{};
    for (std::size_t length = 1; length <= input.size(); ++length) {
        result = parser.parse(input.substr(0, length));
        if (!result || result->has_value()) {
            break;
        }
    }
    return result;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view input{reinterpret_cast<const char*>(data), size};

    http::RequestParser parser{limits};
    const http::ParseResult first = parser.parse(input);
    check(sameResult(first, parseByteByByte(input)));

    // Walk through any pipelined requests that follow.
    std::string_view rest = input;
    http::ParseResult result = first;
    while (result && result->has_value()) {
        const std::size_t consumed = (*result)->consumed;
        check(consumed > 0 && consumed <= rest.size());
        rest.remove_prefix(consumed);
        result = parser.parse(rest);
    }
    if (!result) {
        check(isParseErrorStatus(result.error().status));
    }
    return 0;
}
