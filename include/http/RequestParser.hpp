#pragma once

#include "http/Message.hpp"
#include "http/Status.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <string_view>

namespace http {

struct ParserLimits {
    // The request line with its CRLF, plus any empty lines before it.
    std::size_t maxRequestLineBytes{8 * 1024UZ};
    // Every header field line with its CRLF, plus the blank line ending the
    // section. Trailer fields of a chunked body share this budget.
    std::size_t maxHeaderBytes{8 * 1024UZ};
    // Header and trailer fields together.
    std::size_t maxHeaderCount{100};
    // The body after any chunked decoding.
    std::size_t maxBodyBytes{1024 * 1024UZ};
};

struct ParseError {
    // The status to answer with; the connection is then closed.
    Status status;
    // A fixed description for logs.
    std::string_view message;
};

struct ParsedRequest {
    Request request;
    // How many bytes at the start of the buffer belonged to this request.
    // Anything after them is the start of the next, pipelined, request.
    std::size_t consumed{0};
};

// A complete request, std::nullopt when more bytes are needed, or an error.
using ParseResult = std::expected<std::optional<ParsedRequest>, ParseError>;

// An incremental HTTP/1.1 request parser following RFC 9112. It performs no
// I/O: the caller appends whatever arrives to a buffer and passes the whole
// buffer each time. The parser remembers how far it got, so each byte is
// examined about once however slowly the request trickles in.
class RequestParser {
public:
    explicit RequestParser(ParserLimits limits = {}) noexcept;

    // `buffer` must start with the first byte of the current request and hold
    // every byte received for it so far. After returning a request or an
    // error, the parser expects the next request at the start of the next
    // buffer it is given.
    [[nodiscard]] ParseResult parse(std::string_view buffer);

private:
    enum class State { RequestLine, Headers, Body, ChunkSize, ChunkData, Trailers };
    enum class Progress { Continue, NeedMore, Done };
    using Step = std::expected<Progress, ParseError>;
    // A complete line without its CRLF, std::nullopt if it has not all
    // arrived yet, or an error.
    using LineResult = std::expected<std::optional<std::string_view>, ParseError>;

    [[nodiscard]] Step step(std::string_view buffer);
    [[nodiscard]] Step parseRequestLine(std::string_view buffer);
    [[nodiscard]] Step parseHeaderLine(std::string_view buffer);
    [[nodiscard]] Step finishHeaders();
    [[nodiscard]] Step parseBody(std::string_view buffer);
    [[nodiscard]] Step parseChunkSize(std::string_view buffer);
    [[nodiscard]] Step parseChunkData(std::string_view buffer);
    [[nodiscard]] Step parseTrailerLine(std::string_view buffer);

    [[nodiscard]] LineResult takeLine(
        std::string_view buffer, std::size_t maxLength, ParseError tooLong);
    [[nodiscard]] LineResult takeFieldLine(std::string_view buffer);
    void reset() noexcept;

    ParserLimits limits_;
    State state_{State::RequestLine};
    // The first byte of the buffer not consumed yet.
    std::size_t offset_{0};
    // How far the buffer has been searched for the end of the current line.
    std::size_t scannedTo_{0};
    std::size_t headerBytes_{0};
    std::size_t fieldCount_{0};
    // Body bytes, or bytes of the current chunk, still to come.
    std::size_t remaining_{0};
    Request request_;
};

} // namespace http
