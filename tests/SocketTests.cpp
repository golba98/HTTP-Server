#include "http/Socket.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
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

// POSIX hands out the lowest free descriptor number, so a test must not create
// new descriptors between closing one and checking that it is closed.

bool isOpen(int fd)
{
    errno = 0;
    return ::fcntl(fd, F_GETFD) != -1;
}

bool isClosed(int fd)
{
    errno = 0;
    return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

int intOption(int fd, int level, int name)
{
    int value = 0;
    socklen_t length = sizeof value;
    REQUIRE(::getsockopt(fd, level, name, &value, &length) == 0);
    return value;
}

// The number of descriptors this process has open. The directory iterator
// holds one itself, which cancels out when two counts are compared.
std::size_t openDescriptorCount()
{
    const std::filesystem::directory_iterator entries{"/proc/self/fd"};
    return static_cast<std::size_t>(std::distance(begin(entries), end(entries)));
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

TEST_CASE("TcpListener listens on the requested loopback address", "[Socket]")
{
    const auto listener = TcpListener::bind(loopback, 0);

    CHECK(intOption(listener.fd(), SOL_SOCKET, SO_ACCEPTCONN) == 1);
    CHECK(intOption(listener.fd(), SOL_SOCKET, SO_TYPE) == SOCK_STREAM);
    CHECK(intOption(listener.fd(), SOL_SOCKET, SO_DOMAIN) == AF_INET);
    CHECK(intOption(listener.fd(), SOL_SOCKET, SO_REUSEADDR) != 0);

    sockaddr_in bound{};
    socklen_t length = sizeof bound;
    REQUIRE(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&bound), &length) == 0);
    CHECK(bound.sin_family == AF_INET);
    CHECK(ntohl(bound.sin_addr.s_addr) == INADDR_LOOPBACK);
    CHECK(ntohs(bound.sin_port) == listener.port());
}

TEST_CASE("TcpListener is move-only", "[Socket]")
{
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<TcpListener>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<TcpListener>);

    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<TcpListener>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<TcpListener>);
    STATIC_REQUIRE(std::is_nothrow_destructible_v<TcpListener>);
}

TEST_CASE("TcpListener move construction transfers the socket", "[Socket]")
{
    std::optional<TcpListener> destination;
    int fd = -1;
    std::uint16_t port = 0;
    {
        auto source = TcpListener::bind(loopback, 0);
        fd = source.fd();
        port = source.port();

        destination.emplace(std::move(source));

        CHECK(destination->fd() == fd);
        CHECK(destination->port() == port);
        CHECK(source.fd() == -1);
    }

    // The moved-from source is gone and must not have closed the socket.
    CHECK(isOpen(fd));

    destination.reset();
    CHECK(isClosed(fd));
}

TEST_CASE("TcpListener move assignment closes the old socket and takes the new one", "[Socket]")
{
    auto destination = TcpListener::bind(loopback, 0);
    auto source = TcpListener::bind(loopback, 0);
    const int oldFd = destination.fd();
    const int newFd = source.fd();
    const std::uint16_t newPort = source.port();

    destination = std::move(source);

    CHECK(isClosed(oldFd));
    CHECK(destination.fd() == newFd);
    CHECK(destination.port() == newPort);
    CHECK(source.fd() == -1);
    CHECK(isOpen(newFd));
}

TEST_CASE("A moved-from TcpListener fails cleanly", "[Socket]")
{
    auto source = TcpListener::bind(loopback, 0);
    const TcpListener destination{std::move(source)};

    std::error_code code;
    try {
        static_cast<void>(source.port());
    } catch (const std::system_error& error) {
        code = error.code();
    }
    CHECK(code == std::errc::bad_file_descriptor);

    const auto accepted = source.accept();
    REQUIRE_FALSE(accepted.has_value());
    CHECK(accepted.error() == std::errc::bad_file_descriptor);
}

TEST_CASE("TcpListener destructor closes the socket and releases the port", "[Socket]")
{
    int fd = -1;
    std::uint16_t port = 0;
    {
        const auto listener = TcpListener::bind(loopback, 0);
        fd = listener.fd();
        port = listener.port();
        REQUIRE(isOpen(fd));
    }

    CHECK(isClosed(fd));

    // A live listener would make this fail with EADDRINUSE.
    const auto rebound = TcpListener::bind(loopback, port);
    CHECK(rebound.port() == port);
}

TEST_CASE("TcpListener accepts a configurable backlog", "[Socket]")
{
    // Linux does not report a socket's backlog, and caps it at
    // net.core.somaxconn, so this only checks that each value gives a working
    // listener. Every backlog admits at least one pending connection.
    const int backlog = GENERATE(1, 16, SOMAXCONN);
    CAPTURE(backlog);

    const auto listener = TcpListener::bind(loopback, 0, backlog);
    const auto client = http::connectTcp(loopback, listener.port());
    const auto accepted = listener.accept();

    REQUIRE(accepted.has_value());
    CHECK(accepted->valid());
}

TEST_CASE("SO_REUSEADDR lets a new listener bind while an old connection is closing", "[Socket]")
{
    std::uint16_t port = 0;
    {
        auto [listener, client, server] = connectPair();
        port = listener.port();

        // The server side closes first, so its end of the connection lingers
        // in FIN_WAIT or TIME_WAIT on `port` after everything is destroyed.
        // Without SO_REUSEADDR, binding the port again fails while it does.
        server.reset();
        client.reset();
    }

    const auto rebound = TcpListener::bind(loopback, port);
    CHECK(rebound.port() == port);
}

TEST_CASE("Failed TcpListener::bind leaks no descriptors", "[Socket]")
{
    const auto occupied = TcpListener::bind(loopback, 0);
    const std::size_t before = openDescriptorCount();

    SECTION("address in use")
    {
        std::error_code code;
        try {
            static_cast<void>(TcpListener::bind(loopback, occupied.port()));
        } catch (const std::system_error& error) {
            code = error.code();
        }

        CHECK(code == std::errc::address_in_use);
    }

    SECTION("malformed address")
    {
        CHECK_THROWS_AS(TcpListener::bind("not-an-address", 0), std::invalid_argument);
    }

    CHECK(openDescriptorCount() == before);
}

TEST_CASE("Repeated TcpListener creation and destruction leaks no descriptors", "[Socket]")
{
    const std::size_t before = openDescriptorCount();

    for (int i = 0; i < 1000; ++i) {
        const auto listener = TcpListener::bind(loopback, 0);
        REQUIRE(listener.port() != 0);
    }

    CHECK(openDescriptorCount() == before);
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
