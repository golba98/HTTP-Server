#pragma once

#include "http/FileDescriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>

#include <sys/socket.h>

namespace http {

// A listening IPv4 TCP socket. The socket itself is non-blocking so that a
// connection reset between poll() and accept() cannot stall the caller.
class TcpListener {
public:
    // Binds to `address` (dotted IPv4, e.g. "127.0.0.1") and starts listening.
    // Port 0 asks the kernel for a free port; port() reports the one chosen.
    // Throws std::invalid_argument for a malformed address and
    // std::system_error when a socket call fails.
    [[nodiscard]] static TcpListener bind(
        std::string_view address, std::uint16_t port, int backlog = SOMAXCONN);

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] std::uint16_t port() const;

    // Accepts one pending connection as a blocking, close-on-exec socket.
    // Fails with EAGAIN when no connection is pending.
    [[nodiscard]] std::expected<FileDescriptor, std::error_code> accept() const;

private:
    explicit TcpListener(FileDescriptor fd) noexcept;

    FileDescriptor fd_;
};

// Opens a blocking, close-on-exec connection to address:port. Throws like
// TcpListener::bind.
[[nodiscard]] FileDescriptor connectTcp(std::string_view address, std::uint16_t port);

// The I/O helpers report failures as values because a peer disconnecting is a
// normal event for a server, not an exceptional one. They retry EINTR.

// Writes all of `data`, looping over partial writes. Never raises SIGPIPE.
[[nodiscard]] std::expected<void, std::error_code> sendAll(int socket, std::string_view data);

// Reads up to buffer.size() bytes. A result of 0 means the peer closed.
[[nodiscard]] std::expected<std::size_t, std::error_code> receiveSome(
    int socket, std::span<char> buffer);

// Sends the first `size` bytes of `file` without raising SIGPIPE. Fails with
// io_error if the file turns out to be shorter than `size`.
[[nodiscard]] std::expected<void, std::error_code> sendFile(
    int socket, int file, std::uint64_t size);

} // namespace http
