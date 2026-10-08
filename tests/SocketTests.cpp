#include "http/Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using http::FileDescriptor;
using http::TcpListener;

namespace {

constexpr std::string_view loopback = "127.0.0.1";

struct ConnectedPair {
    TcpListener listener;
    FileDescriptor client;
    FileDescriptor server;
};

ConnectedPair connectPair()
{
    auto listener = TcpListener::bind(loopback, 0);
    auto client = http::connectTcp(loopback, listener.port());
    auto accepted = listener.accept();
    REQUIRE(accepted.has_value());
    return {std::move(listener), std::move(client), std::move(*accepted)};
}

// Reads until `size` bytes have arrived or the peer closes.
std::string receiveExactly(int fd, std::size_t size)
{
    std::string received;
    std::string chunk(64 * 1024, '\0');
    while (received.size() < size) {
        const auto count = http::receiveSome(fd, chunk);
        REQUIRE(count.has_value());
        if (*count == 0) {
            break;
        }
        received.append(chunk, 0, *count);
    }
    return received;
}

std::string patternedData(std::size_t size)
{
    std::string data(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>('a' + (i % 26));
    }
    return data;
}

bool hasCloseOnExec(int fd)
{
    return (::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
}

bool isNonBlocking(int fd)
{
    return (::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
}

} // namespace

TEST_CASE("TcpListener bound to port 0 reports the port the kernel chose", "[Socket]")
{
    const auto listener = TcpListener::bind(loopback, 0);

    CHECK(listener.port() != 0);
    CHECK(listener.fd() >= 0);
}

TEST_CASE("TcpListener sockets are close-on-exec", "[Socket]")
{
    auto [listener, client, server] = connectPair();

    CHECK(hasCloseOnExec(listener.fd()));
    CHECK(hasCloseOnExec(client.get()));
    CHECK(hasCloseOnExec(server.get()));
}

TEST_CASE("TcpListener is non-blocking but accepted sockets block", "[Socket]")
{
    auto [listener, client, server] = connectPair();

    CHECK(isNonBlocking(listener.fd()));
    CHECK_FALSE(isNonBlocking(server.get()));
}

TEST_CASE("TcpListener accept with nothing pending fails with EAGAIN", "[Socket]")
{
    const auto listener = TcpListener::bind(loopback, 0);

    const auto accepted = listener.accept();

    REQUIRE_FALSE(accepted.has_value());
    CHECK(accepted.error() == std::errc::resource_unavailable_try_again);
}

TEST_CASE("TcpListener bind to a port in use throws EADDRINUSE", "[Socket]")
{
    const auto first = TcpListener::bind(loopback, 0);

    std::error_code code;
    try {
        static_cast<void>(TcpListener::bind(loopback, first.port()));
    } catch (const std::system_error& error) {
        code = error.code();
    }

    CHECK(code == std::errc::address_in_use);
}

TEST_CASE("TcpListener bind rejects a malformed address", "[Socket]")
{
    CHECK_THROWS_AS(TcpListener::bind("not-an-address", 0), std::invalid_argument);
    CHECK_THROWS_AS(http::connectTcp("256.0.0.1", 80), std::invalid_argument);
}

TEST_CASE("Connected sockets carry data in both directions", "[Socket]")
{
    auto [listener, client, server] = connectPair();

    REQUIRE(http::sendAll(client.get(), "ping").has_value());
    CHECK(receiveExactly(server.get(), 4) == "ping");

    REQUIRE(http::sendAll(server.get(), "pong").has_value());
    CHECK(receiveExactly(client.get(), 4) == "pong");
}

TEST_CASE("receiveSome returns 0 once the peer has closed", "[Socket]")
{
    auto [listener, client, server] = connectPair();
    client.reset();

    std::string buffer(16, '\0');
    const auto count = http::receiveSome(server.get(), buffer);

    REQUIRE(count.has_value());
    CHECK(*count == 0);
}

TEST_CASE("sendAll delivers a payload larger than the socket buffers", "[Socket]")
{
    auto [listener, client, server] = connectPair();
    const std::string payload = patternedData(4 * 1024 * 1024);

    // The writer blocks until the reader drains the socket, so it needs its own
    // thread; the result comes back through the future.
    auto writer = std::async(
        std::launch::async, [&payload, fd = client.get()] { return http::sendAll(fd, payload); });

    const std::string received = receiveExactly(server.get(), payload.size());

    CHECK(writer.get().has_value());
    CHECK(received == payload);
}

TEST_CASE("sendAll to a closed peer fails instead of raising SIGPIPE", "[Socket]")
{
    auto [listener, client, server] = connectPair();
    client.reset();

    // The first writes can still succeed until the peer's RST arrives. Without
    // MSG_NOSIGNAL a later one would kill the whole test process.
    const std::string chunk(64 * 1024, 'x');
    std::error_code code;
    for (int attempt = 0; attempt < 100 && !code; ++attempt) {
        if (const auto sent = http::sendAll(server.get(), chunk); !sent) {
            code = sent.error();
        }
    }

    CHECK((code == std::errc::broken_pipe || code == std::errc::connection_reset));
}

TEST_CASE("sendFile streams a file's contents to a socket", "[Socket]")
{
    auto [listener, client, server] = connectPair();
    const std::string contents = patternedData(200 * 1024);

    const FileDescriptor file{::memfd_create("sendFile-test", MFD_CLOEXEC)};
    REQUIRE(file.valid());
    REQUIRE(::write(file.get(), contents.data(), contents.size()) ==
        static_cast<ssize_t>(contents.size()));

    SECTION("whole file")
    {
        auto writer = std::async(std::launch::async, [fd = server.get(), &file, &contents] {
            return http::sendFile(fd, file.get(), contents.size());
        });

        const std::string received = receiveExactly(client.get(), contents.size());

        CHECK(writer.get().has_value());
        CHECK(received == contents);
    }

    SECTION("fails when the file is shorter than the requested size")
    {
        auto writer = std::async(std::launch::async, [fd = server.get(), &file, &contents] {
            return http::sendFile(fd, file.get(), contents.size() + 1);
        });

        static_cast<void>(receiveExactly(client.get(), contents.size()));
        const auto result = writer.get();

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == std::errc::io_error);
    }
}
