#pragma once

#include "http/Message.hpp"
#include "http/Socket.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <poll.h>

namespace test {

using namespace std::chrono_literals;

// A response as a client sees it.
struct ClientResponse {
    int status{0};
    std::vector<http::Header> headers;
    std::string body;

    [[nodiscard]] std::optional<std::string> header(std::string_view name) const
    {
        for (const auto& field : headers) {
            if (field.name == name) {
                return field.value;
            }
        }
        return std::nullopt;
    }
};

// Reads whatever arrives within `timeout` and appends it to `buffer`.
// Returns false on EOF, error or timeout.
inline bool receiveMore(int fd, std::string& buffer, std::chrono::milliseconds timeout = 5s)
{
    pollfd ready{fd, POLLIN, 0};
    if (::poll(&ready, 1, static_cast<int>(timeout.count())) != 1) {
        return false;
    }
    std::array<char, 16 * 1024UZ> chunk{};
    const auto count = http::receiveSome(fd, chunk);
    if (!count || *count == 0) {
        return false;
    }
    buffer.append(chunk.data(), *count);
    return true;
}

// Reads one response from `fd`. Bytes beyond it stay in `buffer` for the next
// call. `headRequest` means the response has headers but no body. Returns
// std::nullopt if the connection ends or stalls before the response is whole.
inline std::optional<ClientResponse> readResponse(
    int fd, std::string& buffer, bool headRequest = false)
{
    std::size_t headEnd = 0;
    while ((headEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (!receiveMore(fd, buffer)) {
            return std::nullopt;
        }
    }

    ClientResponse response;
    std::string_view head = std::string_view{buffer}.substr(0, headEnd + 2);
    const std::size_t statusLineEnd = head.find("\r\n");
    const std::string_view statusLine = head.substr(0, statusLineEnd);
    // "HTTP/1.1 200 OK"
    if (!statusLine.starts_with("HTTP/1.1 ") || statusLine.size() < 12) {
        return std::nullopt;
    }
    response.status = std::stoi(std::string{statusLine.substr(9, 3)});
    head.remove_prefix(statusLineEnd + 2);
    while (!head.empty()) {
        const std::size_t lineEnd = head.find("\r\n");
        const std::string_view line = head.substr(0, lineEnd);
        const std::size_t colon = line.find(": ");
        response.headers.push_back(
            {std::string{line.substr(0, colon)}, std::string{line.substr(colon + 2)}});
        head.remove_prefix(lineEnd + 2);
    }
    buffer.erase(0, headEnd + 4);

    const auto length = response.header("Content-Length");
    const std::size_t bodySize = headRequest || !length ? 0 : std::stoul(*length);
    while (buffer.size() < bodySize) {
        if (!receiveMore(fd, buffer)) {
            return std::nullopt;
        }
    }
    response.body = buffer.substr(0, bodySize);
    buffer.erase(0, bodySize);
    return response;
}

// True if the peer closes the connection within `timeout` without sending
// anything more.
inline bool closedByPeer(int fd, std::chrono::milliseconds timeout = 5s)
{
    pollfd ready{fd, POLLIN, 0};
    if (::poll(&ready, 1, static_cast<int>(timeout.count())) != 1) {
        return false;
    }
    std::array<char, 1> byte{};
    const auto count = http::receiveSome(fd, byte);
    // A reset also means the server is gone.
    return !count || *count == 0;
}

} // namespace test
