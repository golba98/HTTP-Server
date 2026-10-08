#pragma once

#include "http/FileDescriptor.hpp"
#include "http/Message.hpp"
#include "http/RequestParser.hpp"
#include "http/Socket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace http {

struct ServerConfig {
    std::string address{"127.0.0.1"};
    std::uint16_t port{8080};
    // Each open connection occupies a worker while it waits for its next
    // request, so there are far more workers than CPU cores.
    std::size_t threadCount{64};
    // Accepted connections waiting for a worker; beyond this they get 503.
    std::size_t queueCapacity{1024};
    // How long a keep-alive connection may sit idle between requests.
    std::chrono::milliseconds idleTimeout{5000};
    // How long a client has to finish sending a request once it has begun,
    // so a client trickling bytes cannot hold a worker indefinitely.
    std::chrono::milliseconds requestTimeout{10000};
    // How long one send may block on a client that has stopped reading.
    std::chrono::milliseconds writeTimeout{10000};
    ParserLimits limits;
    // Log one line per request to std::clog.
    bool accessLog{true};
};

// Produces the response to a request. Called from several threads at once.
using Handler = std::function<Response(const Request&)>;

// An HTTP/1.1 server: one thread accepts connections and hands each to a
// pool of workers, which serve its requests with blocking I/O.
class Server {
public:
    // Binds the listening socket right away, so port() is known before
    // run(). Throws like TcpListener::bind.
    Server(ServerConfig config, Handler handler);

    [[nodiscard]] std::uint16_t port() const;

    // Serves connections until requestStop() is called, then waits for the
    // workers to finish the requests in progress. Call it once.
    void run();

    // Makes run() return. Safe to call from any thread, and from a signal
    // handler.
    void requestStop() noexcept;

private:
    ServerConfig config_;
    Handler handler_;
    TcpListener listener_;
    // An eventfd that becomes readable once a stop is requested and stays
    // readable, so every poll() in the server can watch it.
    FileDescriptor stop_;
};

} // namespace http
