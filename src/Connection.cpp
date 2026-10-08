#include "Connection.hpp"

#include "http/RequestParser.hpp"
#include "http/Socket.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <exception>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <syncstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace http::detail {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// How long to keep discarding input after deciding to close a connection.
constexpr auto lingerTimeout = 1s;
constexpr std::size_t lingerMaxBytes = 64 * 1024UZ;

enum class Wait { Readable, TimedOut, Stopping, Failed };

// Waits until `fd` has input (or EOF), `deadline` passes, or a stop is
// requested through `stopFd`.
Wait waitReadable(int fd, int stopFd, Clock::time_point deadline)
{
    for (;;) {
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining <= 0ms) {
            return Wait::TimedOut;
        }
        std::array<pollfd, 2> fds{{{fd, POLLIN, 0}, {stopFd, POLLIN, 0}}};
        const auto timeout =
            static_cast<int>(std::min<std::chrono::milliseconds::rep>(remaining.count(), INT_MAX));
        const int ready = ::poll(fds.data(), fds.size(), timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Wait::Failed;
        }
        if (fds[1].revents != 0) {
            return Wait::Stopping;
        }
        if (fds[0].revents != 0) {
            return Wait::Readable;
        }
        // poll() timed out; the loop re-checks the deadline.
    }
}

bool stopRequested(int stopFd)
{
    pollfd stop{stopFd, POLLIN, 0};
    return ::poll(&stop, 1, 0) == 1;
}

void configureSocket(int fd, std::chrono::milliseconds writeTimeout)
{
    // Headers and a file body go out in separate writes; without TCP_NODELAY,
    // Nagle's algorithm would hold the second back until the client's
    // delayed ACK, adding about 40 ms to every such response.
    const int enable = 1;
    static_cast<void>(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof enable));

    // Makes a blocked send() give up on a client that has stopped reading.
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(writeTimeout);
    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(seconds.count());
    timeout.tv_usec = static_cast<suseconds_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(writeTimeout - seconds).count());
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout));
}

// Closing a socket that still has unread input makes the kernel send a RST,
// which can destroy a response the client has not read yet. Shutting down our
// side first and briefly discarding input lets the client see the response.
void lingeringClose(int fd, int stopFd)
{
    static_cast<void>(::shutdown(fd, SHUT_WR));
    const auto deadline = Clock::now() + lingerTimeout;
    std::array<char, 4096> discard{};
    std::size_t discarded = 0;
    while (discarded < lingerMaxBytes && waitReadable(fd, stopFd, deadline) == Wait::Readable) {
        const auto count = receiveSome(fd, discard);
        if (!count || *count == 0) {
            break;
        }
        discarded += *count;
    }
}

std::string peerName(int fd)
{
    sockaddr_in address{};
    socklen_t length = sizeof address;
    std::array<char, INET_ADDRSTRLEN> text{};
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0 ||
        ::inet_ntop(AF_INET, &address.sin_addr, text.data(), text.size()) == nullptr) {
        return "-";
    }
    return std::format("{}:{}", text.data(), ntohs(address.sin_port));
}

std::string_view versionName(Version version) noexcept
{
    return version == Version::Http10 ? "HTTP/1.0" : "HTTP/1.1";
}

// Sends the head and, unless `headOnly`, the body. Returns false if the
// connection failed.
bool sendResponse(int fd, const Response& response, bool headOnly)
{
    std::string head = serializeHead(response);
    const auto* text = std::get_if<std::string>(&response.body);
    if (headOnly) {
        return sendAll(fd, head).has_value();
    }
    if (text != nullptr) {
        // One write for small text responses.
        head += *text;
        return sendAll(fd, head).has_value();
    }
    const auto& file = std::get<FileBody>(response.body);
    return sendAll(fd, head).has_value() && sendFile(fd, file.file.get(), file.size).has_value();
}

// Answers a request the parser rejected or that took too long. The
// connection is always closed afterwards.
void sendError(int fd, Status status)
{
    Response response = makeErrorResponse(status);
    response.headers.push_back({"Date", httpDate(std::chrono::system_clock::now())});
    response.headers.push_back({"Connection", "close"});
    static_cast<void>(sendResponse(fd, response, false));
}

Response callHandler(const Handler& handler, const Request& request)
{
    try {
        return handler(request);
    } catch (const std::exception& error) {
        std::osyncstream{std::cerr} << "http-server: handler failed for " << request.target << ": "
                                    << error.what() << '\n';
    } catch (...) {
        std::osyncstream{std::cerr} << "http-server: handler failed for " << request.target << '\n';
    }
    return makeErrorResponse(Status::InternalServerError);
}

class ConnectionLog {
public:
    ConnectionLog(int fd, bool enabled)
        : peer_{enabled ? peerName(fd) : std::string{}}
        , enabled_{enabled}
    {
    }

    void request(const Request& request, const Response& response) const
    {
        if (enabled_) {
            std::osyncstream{std::clog} << std::format("{} \"{} {} {}\" {} {}\n", peer_,
                request.method, request.target, versionName(request.version),
                statusCode(response.status), response.contentLength());
        }
    }

    void rejected(Status status, std::string_view reason) const
    {
        if (enabled_) {
            std::osyncstream{std::clog}
                << std::format("{} \"-\" {} ({})\n", peer_, statusCode(status), reason);
        }
    }

private:
    std::string peer_;
    bool enabled_;
};

} // namespace

void serveConnection(
    FileDescriptor connection, const Handler& handler, const ServerConfig& config, int stopFd)
{
    const int fd = connection.get();
    configureSocket(fd, config.writeTimeout);
    const ConnectionLog log{fd, config.accessLog};

    RequestParser parser{config.limits};
    std::string buffer;
    std::array<char, 16 * 1024UZ> chunk{};
    // Set while part of a request has arrived: the request must be complete
    // by then. Otherwise the connection is idle until idleDeadline.
    std::optional<Clock::time_point> requestDeadline;
    Clock::time_point idleDeadline = Clock::now() + config.idleTimeout;

    for (;;) {
        // Parse before reading: pipelined requests may already be buffered.
        auto parsed = parser.parse(buffer);
        if (!parsed) {
            log.rejected(parsed.error().status, parsed.error().message);
            sendError(fd, parsed.error().status);
            lingeringClose(fd, stopFd);
            return;
        }

        if (*parsed) {
            const Request& request = (*parsed)->request;
            Response response = callHandler(handler, request);

            const bool keepOpen = request.keepAlive() && !stopRequested(stopFd);
            response.headers.push_back({"Date", httpDate(std::chrono::system_clock::now())});
            if (!keepOpen) {
                response.headers.push_back({"Connection", "close"});
            } else if (request.version == Version::Http10) {
                response.headers.push_back({"Connection", "keep-alive"});
            }

            const bool sent = sendResponse(fd, response, request.method == "HEAD");
            log.request(request, response);
            if (!sent) {
                return;
            }
            if (!keepOpen) {
                lingeringClose(fd, stopFd);
                return;
            }

            buffer.erase(0, (*parsed)->consumed);
            const auto now = Clock::now();
            idleDeadline = now + config.idleTimeout;
            requestDeadline.reset();
            if (!buffer.empty()) {
                requestDeadline = now + config.requestTimeout;
            }
            continue;
        }

        switch (waitReadable(fd, stopFd, requestDeadline.value_or(idleDeadline))) {
        case Wait::Readable:
            break;
        case Wait::TimedOut:
            // An idle connection just closes; a half-sent request gets 408.
            if (requestDeadline) {
                log.rejected(Status::RequestTimeout, "request not completed in time");
                sendError(fd, Status::RequestTimeout);
                lingeringClose(fd, stopFd);
            }
            return;
        case Wait::Stopping:
        case Wait::Failed:
            return;
        }

        const auto count = receiveSome(fd, chunk);
        if (!count || *count == 0) {
            return; // The client closed or reset the connection.
        }
        buffer.append(chunk.data(), *count);
        if (!requestDeadline) {
            requestDeadline = Clock::now() + config.requestTimeout;
        }
    }
}

} // namespace http::detail
