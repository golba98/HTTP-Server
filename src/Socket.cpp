#include "http/Socket.hpp"

#include "SystemError.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

namespace http {

namespace {

sockaddr_in makeAddress(std::string_view address, std::uint16_t port)
{
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(port);
    // inet_pton needs a NUL-terminated string.
    if (::inet_pton(AF_INET, std::string{address}.c_str(), &result.sin_addr) != 1) {
        throw std::invalid_argument{"not an IPv4 address: " + std::string{address}};
    }
    return result;
}

FileDescriptor openTcpSocket(int extraFlags)
{
    FileDescriptor fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | extraFlags, 0)};
    if (!fd) {
        detail::throwSystemError("socket");
    }
    return fd;
}

} // namespace

TcpListener::TcpListener(FileDescriptor fd) noexcept
    : fd_{std::move(fd)}
{
}

TcpListener TcpListener::bind(std::string_view address, std::uint16_t port, int backlog)
{
    const sockaddr_in socketAddress = makeAddress(address, port);
    FileDescriptor fd = openTcpSocket(SOCK_NONBLOCK);

    // Lets a restarted server bind while old connections sit in TIME_WAIT. It
    // does not allow two live listeners on one port.
    const int enable = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable) != 0) {
        detail::throwSystemError("setsockopt(SO_REUSEADDR)");
    }
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&socketAddress), sizeof socketAddress) !=
        0) {
        detail::throwSystemError("bind");
    }
    if (::listen(fd.get(), backlog) != 0) {
        detail::throwSystemError("listen");
    }
    return TcpListener{std::move(fd)};
}

int TcpListener::fd() const noexcept
{
    return fd_.get();
}

std::uint16_t TcpListener::port() const
{
    sockaddr_in socketAddress{};
    socklen_t length = sizeof socketAddress;
    if (::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&socketAddress), &length) != 0) {
        detail::throwSystemError("getsockname");
    }
    return ntohs(socketAddress.sin_port);
}

std::expected<FileDescriptor, std::error_code> TcpListener::accept() const
{
    for (;;) {
        // Linux does not pass O_NONBLOCK on to accepted sockets, so they block.
        const int connection = ::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (connection >= 0) {
            return FileDescriptor{connection};
        }
        if (errno != EINTR) {
            return std::unexpected{detail::lastError()};
        }
    }
}

FileDescriptor connectTcp(std::string_view address, std::uint16_t port)
{
    const sockaddr_in socketAddress = makeAddress(address, port);
    FileDescriptor fd = openTcpSocket(0);
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&socketAddress),
            sizeof socketAddress) != 0) {
        detail::throwSystemError("connect");
    }
    return fd;
}

std::expected<void, std::error_code> sendAll(int socket, std::string_view data)
{
    while (!data.empty()) {
        // MSG_NOSIGNAL turns a write to a closed peer into EPIPE instead of a
        // SIGPIPE that would terminate the process.
        const ssize_t sent = ::send(socket, data.data(), data.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected{detail::lastError()};
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return {};
}

std::expected<std::size_t, std::error_code> receiveSome(int socket, std::span<char> buffer)
{
    for (;;) {
        const ssize_t received = ::recv(socket, buffer.data(), buffer.size(), 0);
        if (received >= 0) {
            return static_cast<std::size_t>(received);
        }
        if (errno != EINTR) {
            return std::unexpected{detail::lastError()};
        }
    }
}

std::expected<void, std::error_code> sendFile(int socket, int file, std::uint64_t size)
{
    // sendfile(2) would avoid this copy, but it has no MSG_NOSIGNAL equivalent:
    // a client that disconnects mid-download would raise SIGPIPE. pread() plus
    // send() keeps the "never SIGPIPE" guarantee without process-wide signal
    // settings.
    std::array<char, 64 * 1024UZ> chunk{};
    std::uint64_t offset = 0;
    while (offset < size) {
        const auto wanted =
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), size - offset));
        const ssize_t got = ::pread(file, chunk.data(), wanted, static_cast<off_t>(offset));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected{detail::lastError()};
        }
        if (got == 0) {
            // The file shrank after its size was taken; the promised length
            // can no longer be delivered.
            return std::unexpected{std::make_error_code(std::errc::io_error)};
        }
        const auto count = static_cast<std::size_t>(got);
        if (auto sent = sendAll(socket, {chunk.data(), count}); !sent) {
            return sent;
        }
        offset += count;
    }
    return {};
}

} // namespace http
