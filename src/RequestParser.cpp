#include "http/RequestParser.hpp"

#include "Ascii.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace http {

namespace {

constexpr auto npos = std::string_view::npos;

// Chunk extensions are ignored, but a chunk-size line is still bounded.
constexpr std::size_t maxChunkSizeLineBytes = 1024;

std::unexpected<ParseError> fail(Status status, std::string_view message)
{
    return std::unexpected{ParseError{status, message}};
}

// The request-target may only contain visible ASCII; anything else must be
// percent-encoded by the client.
bool isTargetChar(char c) noexcept
{
    const auto byte = static_cast<unsigned char>(c);
    return byte > 0x20 && byte < 0x7f;
}

// HTAB, SP, VCHAR and obs-text (RFC 9110, section 5.5): everything except the
// other control characters.
bool isFieldValueChar(char c) noexcept
{
    const auto byte = static_cast<unsigned char>(c);
    return byte == '\t' || (byte >= 0x20 && byte != 0x7f);
}

std::uint64_t hexValue(char c) noexcept
{
    if (detail::isDigit(c)) {
        return static_cast<std::uint64_t>(c - '0');
    }
    return static_cast<std::uint64_t>(detail::toLower(c) - 'a') + 10;
}

// Content-Length is 1*DIGIT. Unlike std::from_chars, this rejects a sign.
std::optional<std::uint64_t> parseDecimal(std::string_view text) noexcept
{
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (!detail::isDigit(c)) {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

std::expected<Version, ParseError> parseVersion(std::string_view text)
{
    // HTTP-version is case-sensitive: "HTTP/" DIGIT "." DIGIT.
    if (text.size() != 8 || !text.starts_with("HTTP/") || !detail::isDigit(text[5]) ||
        text[6] != '.' || !detail::isDigit(text[7])) {
        return fail(Status::BadRequest, "malformed HTTP version");
    }
    if (text[5] != '1') {
        return fail(Status::HttpVersionNotSupported, "unsupported HTTP major version");
    }
    // A later 1.x minor version is handled as the highest one we know.
    return text[7] == '0' ? Version::Http10 : Version::Http11;
}

std::expected<Header, ParseError> parseFieldLine(std::string_view line)
{
    if (detail::isOws(line.front())) {
        return fail(Status::BadRequest, "obsolete line folding");
    }
    const std::size_t colon = line.find(':');
    if (colon == npos) {
        return fail(Status::BadRequest, "header field without a colon");
    }
    // A token cannot contain whitespace, which also rejects "Name : value".
    const std::string_view name = line.substr(0, colon);
    if (!detail::isToken(name)) {
        return fail(Status::BadRequest, "invalid header field name");
    }
    const std::string_view value = detail::trimOws(line.substr(colon + 1));
    if (!std::ranges::all_of(value, isFieldValueChar)) {
        return fail(Status::BadRequest, "invalid character in header field value");
    }
    return Header{std::string{name}, std::string{value}};
}

} // namespace

RequestParser::RequestParser(ParserLimits limits) noexcept
    : limits_{limits}
{
}

ParseResult RequestParser::parse(std::string_view buffer)
{
    for (;;) {
        const Step progress = step(buffer);
        if (!progress) {
            reset();
            return std::unexpected{progress.error()};
        }
        switch (*progress) {
        case Progress::Continue:
            break;
        case Progress::NeedMore:
            return std::nullopt;
        case Progress::Done: {
            ParsedRequest parsed{std::move(request_), offset_};
            reset();
            return parsed;
        }
        }
    }
}

RequestParser::Step RequestParser::step(std::string_view buffer)
{
    switch (state_) {
    case State::RequestLine:
        return parseRequestLine(buffer);
    case State::Headers:
        return parseHeaderLine(buffer);
    case State::Body:
        return parseBody(buffer);
    case State::ChunkSize:
        return parseChunkSize(buffer);
    case State::ChunkData:
        return parseChunkData(buffer);
    case State::Trailers:
        return parseTrailerLine(buffer);
    }
    std::unreachable();
}

RequestParser::LineResult RequestParser::takeLine(
    std::string_view buffer, std::size_t maxLength, ParseError tooLong)
{
    const std::size_t newline = buffer.find('\n', std::max(offset_, scannedTo_));
    if (newline == npos) {
        // Remember the search position so the next call only looks at new
        // bytes. The finished line will be at least one byte longer than what
        // has arrived, so it can already be too long.
        scannedTo_ = buffer.size();
        if (buffer.size() - offset_ >= maxLength) {
            return std::unexpected{tooLong};
        }
        return std::nullopt;
    }
    if (newline + 1 - offset_ > maxLength) {
        return std::unexpected{tooLong};
    }
    // Only CRLF ends a line. Accepting a bare LF invites request smuggling
    // through software that disagrees about where a line ends.
    if (newline == offset_ || buffer[newline - 1] != '\r') {
        return fail(Status::BadRequest, "line not terminated by CRLF");
    }
    const std::string_view line = buffer.substr(offset_, newline - 1 - offset_);
    offset_ = newline + 1;
    return line;
}

RequestParser::LineResult RequestParser::takeFieldLine(std::string_view buffer)
{
    const std::size_t lineStart = offset_;
    auto line = takeLine(buffer, limits_.maxHeaderBytes - headerBytes_,
        {Status::RequestHeaderFieldsTooLarge, "header section too large"});
    if (line && *line) {
        headerBytes_ += offset_ - lineStart;
        if (!(*line)->empty() && fieldCount_++ == limits_.maxHeaderCount) {
            return fail(Status::RequestHeaderFieldsTooLarge, "too many header fields");
        }
    }
    return line;
}

RequestParser::Step RequestParser::parseRequestLine(std::string_view buffer)
{
    // Empty lines skipped before the request line count toward its limit, so
    // offset_ never exceeds it.
    const auto line = takeLine(buffer, limits_.maxRequestLineBytes - offset_,
        {Status::UriTooLong, "request line too long"});
    if (!line) {
        return std::unexpected{line.error()};
    }
    if (!*line) {
        return Progress::NeedMore;
    }
    const std::string_view text = **line;
    // RFC 9112, section 2.2: ignore empty lines received before the request.
    if (text.empty()) {
        return Progress::Continue;
    }

    // method SP request-target SP HTTP-version, with exactly two spaces.
    const std::size_t firstSpace = text.find(' ');
    const std::size_t secondSpace = firstSpace == npos ? npos : text.find(' ', firstSpace + 1);
    if (secondSpace == npos || text.find(' ', secondSpace + 1) != npos) {
        return fail(Status::BadRequest, "malformed request line");
    }
    const std::string_view method = text.substr(0, firstSpace);
    const std::string_view target = text.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    if (!detail::isToken(method)) {
        return fail(Status::BadRequest, "invalid method");
    }
    if (target.empty() || !std::ranges::all_of(target, isTargetChar)) {
        return fail(Status::BadRequest, "invalid request target");
    }
    const auto version = parseVersion(text.substr(secondSpace + 1));
    if (!version) {
        return std::unexpected{version.error()};
    }

    request_.method = method;
    request_.target = target;
    request_.version = *version;
    state_ = State::Headers;
    return Progress::Continue;
}

RequestParser::Step RequestParser::parseHeaderLine(std::string_view buffer)
{
    const auto line = takeFieldLine(buffer);
    if (!line) {
        return std::unexpected{line.error()};
    }
    if (!*line) {
        return Progress::NeedMore;
    }
    if ((*line)->empty()) {
        return finishHeaders();
    }
    auto field = parseFieldLine(**line);
    if (!field) {
        return std::unexpected{field.error()};
    }
    request_.headers.push_back(std::move(*field));
    return Progress::Continue;
}

RequestParser::Step RequestParser::finishHeaders()
{
    std::size_t hostCount = 0;
    std::size_t lengthCount = 0;
    std::string_view contentLength;
    bool hasTransferEncoding = false;
    std::size_t codingCount = 0;
    std::string_view lastCoding;

    for (const Header& field : request_.headers) {
        if (detail::equalsIgnoreCase(field.name, "Host")) {
            ++hostCount;
        } else if (detail::equalsIgnoreCase(field.name, "Content-Length")) {
            ++lengthCount;
            contentLength = field.value;
        } else if (detail::equalsIgnoreCase(field.name, "Transfer-Encoding")) {
            hasTransferEncoding = true;
            detail::forEachListElement(field.value, [&](std::string_view coding) {
                ++codingCount;
                lastCoding = coding;
            });
        }
    }

    if (hostCount > 1 || (hostCount == 0 && request_.version == Version::Http11)) {
        return fail(Status::BadRequest, "HTTP/1.1 requires exactly one Host field");
    }

    // RFC 9112, section 6.3. Framing ambiguities are rejected outright because
    // a proxy that resolves them differently would see a different request
    // boundary (request smuggling).
    if (hasTransferEncoding) {
        if (request_.version == Version::Http10) {
            return fail(Status::BadRequest, "Transfer-Encoding in an HTTP/1.0 request");
        }
        if (lengthCount != 0) {
            return fail(Status::BadRequest, "both Transfer-Encoding and Content-Length");
        }
        if (codingCount == 0 || !detail::equalsIgnoreCase(lastCoding, "chunked")) {
            return fail(Status::BadRequest, "chunked is not the final transfer coding");
        }
        if (codingCount > 1) {
            return fail(Status::NotImplemented, "unsupported transfer coding");
        }
        state_ = State::ChunkSize;
        return Progress::Continue;
    }

    if (lengthCount > 1) {
        return fail(Status::BadRequest, "repeated Content-Length");
    }
    if (lengthCount == 0) {
        return Progress::Done;
    }

    const auto length = parseDecimal(contentLength);
    if (!length) {
        return fail(Status::BadRequest, "invalid Content-Length");
    }
    if (*length > limits_.maxBodyBytes) {
        return fail(Status::ContentTooLarge, "body too large");
    }
    remaining_ = static_cast<std::size_t>(*length);
    state_ = State::Body;
    return Progress::Continue;
}

RequestParser::Step RequestParser::parseBody(std::string_view buffer)
{
    if (buffer.size() - offset_ < remaining_) {
        return Progress::NeedMore;
    }
    request_.body.assign(buffer.substr(offset_, remaining_));
    offset_ += remaining_;
    return Progress::Done;
}

RequestParser::Step RequestParser::parseChunkSize(std::string_view buffer)
{
    const auto line =
        takeLine(buffer, maxChunkSizeLineBytes, {Status::BadRequest, "chunk size line too long"});
    if (!line) {
        return std::unexpected{line.error()};
    }
    if (!*line) {
        return Progress::NeedMore;
    }
    const std::string_view text = **line;

    std::uint64_t size = 0;
    std::size_t digits = 0;
    for (; digits < text.size() && detail::isHexDigit(text[digits]); ++digits) {
        if (size > std::numeric_limits<std::uint64_t>::max() >> 4) {
            return fail(Status::BadRequest, "chunk size overflows");
        }
        size = (size << 4) | hexValue(text[digits]);
    }
    if (digits == 0) {
        return fail(Status::BadRequest, "missing chunk size");
    }
    const std::string_view extension = detail::trimOws(text.substr(digits));
    if (!extension.empty() &&
        (extension.front() != ';' || !std::ranges::all_of(extension, isFieldValueChar))) {
        return fail(Status::BadRequest, "malformed chunk extension");
    }
    if (size > limits_.maxBodyBytes - request_.body.size()) {
        return fail(Status::ContentTooLarge, "body too large");
    }

    if (size == 0) {
        state_ = State::Trailers;
    } else {
        remaining_ = static_cast<std::size_t>(size);
        state_ = State::ChunkData;
    }
    return Progress::Continue;
}

RequestParser::Step RequestParser::parseChunkData(std::string_view buffer)
{
    // Wait for the data and the CRLF after it, so nothing is looked at twice.
    if (buffer.size() - offset_ < remaining_ + 2) {
        return Progress::NeedMore;
    }
    request_.body.append(buffer.substr(offset_, remaining_));
    offset_ += remaining_;
    if (buffer.substr(offset_, 2) != "\r\n") {
        return fail(Status::BadRequest, "chunk data not followed by CRLF");
    }
    offset_ += 2;
    state_ = State::ChunkSize;
    return Progress::Continue;
}

RequestParser::Step RequestParser::parseTrailerLine(std::string_view buffer)
{
    const auto line = takeFieldLine(buffer);
    if (!line) {
        return std::unexpected{line.error()};
    }
    if (!*line) {
        return Progress::NeedMore;
    }
    if ((*line)->empty()) {
        return Progress::Done;
    }
    // Trailer fields are validated but dropped: nothing here needs them, and
    // merging them into the headers could let a client change fields such
    // as Host after the fact.
    if (const auto field = parseFieldLine(**line); !field) {
        return std::unexpected{field.error()};
    }
    return Progress::Continue;
}

void RequestParser::reset() noexcept
{
    state_ = State::RequestLine;
    offset_ = 0;
    scannedTo_ = 0;
    headerBytes_ = 0;
    fieldCount_ = 0;
    remaining_ = 0;
    request_ = Request{};
}

} // namespace http
