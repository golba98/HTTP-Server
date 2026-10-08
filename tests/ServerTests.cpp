#include "http/Server.hpp"
#include "http/StaticFiles.hpp"

#include "support/TempDir.hpp"
#include "support/TestClient.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>

using http::Request;
using http::Response;
using http::ServerConfig;
using namespace std::chrono_literals;

namespace {

ServerConfig testConfig()
{
    ServerConfig config;
    config.address = "127.0.0.1";
    config.port = 0;
    config.threadCount = 4;
    config.accessLog = false;
    return config;
}

// Answers with "<method> <target> <body>".
Response echo(const Request& request)
{
    Response response;
    response.headers = {{"Content-Type", "text/plain"}};
    response.body = request.method + " " + request.target + " " + request.body;
    return response;
}

// A server running on its own thread for the duration of a test.
class RunningServer {
public:
    explicit RunningServer(ServerConfig config = testConfig(), http::Handler handler = echo)
        : server_{std::move(config), std::move(handler)}
        , finished_{std::async(std::launch::async, [this] { server_.run(); })}
    {
    }

    ~RunningServer()
    {
        server_.requestStop();
        // Swallow errors here; tests that care call finished() themselves.
        if (finished_.valid()) {
            finished_.wait();
        }
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;
    RunningServer(RunningServer&&) = delete;
    RunningServer& operator=(RunningServer&&) = delete;

    [[nodiscard]] http::FileDescriptor connect() const
    {
        return http::connectTcp("127.0.0.1", server_.port());
    }

    http::Server& server() noexcept
    {
        return server_;
    }

    std::future<void>& finished() noexcept
    {
        return finished_;
    }

private:
    http::Server server_;
    std::future<void> finished_;
};

void send(const http::FileDescriptor& connection, std::string_view data)
{
    REQUIRE(http::sendAll(connection.get(), data).has_value());
}

constexpr std::string_view simpleGet = "GET / HTTP/1.1\r\nHost: test\r\n\r\n";

} // namespace

TEST_CASE("Server answers a request", "[Server]")
{
    RunningServer running;
    const auto connection = running.connect();
    send(connection, "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n");

    std::string buffer;
    const auto response = test::readResponse(connection.get(), buffer);

    REQUIRE(response.has_value());
    CHECK(response->status == 200);
    CHECK(response->body == "GET /hello ");
    CHECK(response->header("Content-Length") == "11");
    CHECK(response->header("Date").has_value());
    CHECK_FALSE(response->header("Connection").has_value());
}

TEST_CASE("Server keeps HTTP/1.1 connections open between requests", "[Server]")
{
    RunningServer running;
    const auto connection = running.connect();
    std::string buffer;

    for (const std::string_view target : {"/one", "/two", "/three"}) {
        send(connection, "GET " + std::string{target} + " HTTP/1.1\r\nHost: test\r\n\r\n");
        const auto response = test::readResponse(connection.get(), buffer);
        REQUIRE(response.has_value());
        CHECK(response->body == "GET " + std::string{target} + " ");
    }
}

TEST_CASE("Server answers pipelined requests in order", "[Server]")
{
    RunningServer running;
    const auto connection = running.connect();
    send(connection,
        "GET /one HTTP/1.1\r\nHost: test\r\n\r\n"
        "POST /two HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nbody\r\n0\r\n\r\n"
        "GET /three HTTP/1.1\r\nHost: test\r\n\r\n");

    std::string buffer;
    const auto one = test::readResponse(connection.get(), buffer);
    const auto two = test::readResponse(connection.get(), buffer);
    const auto three = test::readResponse(connection.get(), buffer);

    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    REQUIRE(three.has_value());
    CHECK(one->body == "GET /one ");
    CHECK(two->body == "POST /two body");
    CHECK(three->body == "GET /three ");
}

TEST_CASE("Server closes the connection when the client asks", "[Server]")
{
    RunningServer running;
    const auto connection = running.connect();

    SECTION("with Connection: close")
    {
        send(connection, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    }

    SECTION("with HTTP/1.0")
    {
        send(connection, "GET / HTTP/1.0\r\n\r\n");
    }

    std::string buffer;
    const auto response = test::readResponse(connection.get(), buffer);
    REQUIRE(response.has_value());
    CHECK(response->header("Connection") == "close");
    CHECK(test::closedByPeer(connection.get()));
}

TEST_CASE("Server keeps HTTP/1.0 keep-alive connections open", "[Server]")
{
    RunningServer running;
    const auto connection = running.connect();
    std::string buffer;

    for (int i = 0; i < 2; ++i) {
        send(connection, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
        const auto response = test::readResponse(connection.get(), buffer);
        REQUIRE(response.has_value());
        CHECK(response->header("Connection") == "keep-alive");
    }
}

TEST_CASE("Server sends no body for HEAD", "[Server]")
{
    RunningServer running;
    const auto connection = running.connect();
    std::string buffer;

    send(connection, "HEAD /page HTTP/1.1\r\nHost: test\r\n\r\n");
    const auto head = test::readResponse(connection.get(), buffer, true);
    REQUIRE(head.has_value());
    CHECK(head->header("Content-Length") == "11");

    // Any stray body bytes would corrupt the next response on the connection.
    send(connection, simpleGet);
    const auto next = test::readResponse(connection.get(), buffer);
    REQUIRE(next.has_value());
    CHECK(next->status == 200);
    CHECK(next->body == "GET / ");
}

TEST_CASE("Server answers malformed requests and closes", "[Server]")
{
    ServerConfig config = testConfig();
    config.limits.maxHeaderBytes = 64;
    RunningServer running{config};
    const auto connection = running.connect();

    int expected = 0;
    SECTION("with 400 for bad syntax")
    {
        send(connection, "GET / HTTP/1.1\r\nBad Header\r\n\r\n");
        expected = 400;
    }

    SECTION("with 431 for an oversized header section")
    {
        send(connection,
            "GET / HTTP/1.1\r\nHost: test\r\nX-Big: " + std::string(100, 'x') + "\r\n\r\n");
        expected = 431;
    }

    std::string buffer;
    const auto response = test::readResponse(connection.get(), buffer);
    REQUIRE(response.has_value());
    CHECK(response->status == expected);
    CHECK(response->header("Connection") == "close");
    CHECK(test::closedByPeer(connection.get()));
}

TEST_CASE("Server answers 500 when the handler throws", "[Server]")
{
    RunningServer running{testConfig(), [](const Request& request) -> Response {
                              if (request.target == "/fail") {
                                  throw std::runtime_error{"expected by the test"};
                              }
                              return echo(request);
                          }};
    const auto connection = running.connect();
    std::string buffer;

    send(connection, "GET /fail HTTP/1.1\r\nHost: test\r\n\r\n");
    const auto failed = test::readResponse(connection.get(), buffer);
    REQUIRE(failed.has_value());
    CHECK(failed->status == 500);

    // The connection survives the handler's failure.
    send(connection, simpleGet);
    const auto next = test::readResponse(connection.get(), buffer);
    REQUIRE(next.has_value());
    CHECK(next->status == 200);
}

TEST_CASE("Server timeouts", "[Server]")
{
    ServerConfig config = testConfig();
    config.idleTimeout = 200ms;
    config.requestTimeout = 200ms;
    RunningServer running{config};
    const auto connection = running.connect();

    SECTION("close an idle connection silently")
    {
        const auto start = std::chrono::steady_clock::now();
        CHECK(test::closedByPeer(connection.get()));
        CHECK(std::chrono::steady_clock::now() - start >= 150ms);
    }

    SECTION("answer 408 to a request that stops arriving")
    {
        send(connection, "GET / HTTP/1.1\r\nHost:");

        std::string buffer;
        const auto response = test::readResponse(connection.get(), buffer);
        REQUIRE(response.has_value());
        CHECK(response->status == 408);
        CHECK(test::closedByPeer(connection.get()));
    }
}

TEST_CASE("Server answers 503 when every worker is busy", "[Server]")
{
    ServerConfig config = testConfig();
    config.threadCount = 1;
    config.queueCapacity = 1;
    config.idleTimeout = 10s;
    RunningServer running{config};

    // The only worker is now waiting for a second request on `busy`.
    const auto busy = running.connect();
    send(busy, simpleGet);
    std::string busyBuffer;
    REQUIRE(test::readResponse(busy.get(), busyBuffer).has_value());

    // Connections are accepted in order: this one fills the queue...
    const auto queued = running.connect();
    // ...and this one is turned away.
    const auto rejected = running.connect();

    std::string buffer;
    const auto response = test::readResponse(rejected.get(), buffer);
    REQUIRE(response.has_value());
    CHECK(response->status == 503);
    CHECK(response->header("Connection") == "close");
}

TEST_CASE("Server stops promptly while connections are idle", "[Server]")
{
    ServerConfig config = testConfig();
    config.idleTimeout = 30s;
    RunningServer running{config};
    const auto connection = running.connect();
    send(connection, simpleGet);
    std::string buffer;
    REQUIRE(test::readResponse(connection.get(), buffer).has_value());

    running.server().requestStop();

    REQUIRE(running.finished().wait_for(2s) == std::future_status::ready);
    CHECK_NOTHROW(running.finished().get());
    CHECK(test::closedByPeer(connection.get(), 1s));
}

TEST_CASE("Server streams files from a StaticFileHandler", "[Server]")
{
    test::TempDir root;
    std::string contents(1024 * 1024 + 7, '\0');
    for (std::size_t i = 0; i < contents.size(); ++i) {
        contents[i] = static_cast<char>('a' + (i % 26));
    }
    root.write("big.txt", contents);
    const http::StaticFileHandler files{root.path()};
    RunningServer running{
        testConfig(), [&files](const Request& request) { return files.handle(request); }};
    const auto connection = running.connect();

    send(connection, "GET /big.txt HTTP/1.1\r\nHost: test\r\n\r\n");
    std::string buffer;
    const auto response = test::readResponse(connection.get(), buffer);

    REQUIRE(response.has_value());
    CHECK(response->status == 200);
    CHECK(response->header("Content-Type") == "text/plain; charset=utf-8");
    CHECK(response->body == contents);
}
