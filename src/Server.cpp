#include "http/Server.hpp"

#include "Connection.hpp"
#include "SystemError.hpp"
#include "http/ThreadPool.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <syncstream>
#include <system_error>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace http {

namespace {

using namespace std::chrono_literals;

// Errors after which accept() can simply be called again. Linux also reports
// pending network errors on the new socket through accept() (see accept(2)).
bool isTransientAcceptError(std::error_code error)
{
    switch (error.value()) {
    case EAGAIN:
    case ECONNABORTED:
    case EINTR:
    case EPROTO:
    case ENETDOWN:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
        return true;
    default:
        return false;
    }
}

// Errors caused by running out of descriptors or memory. They clear up as
// other connections close, so the server pauses instead of giving up.
bool isResourceAcceptError(std::error_code error)
{
    switch (error.value()) {
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
        return true;
    default:
        return false;
    }
}

// Tells a client the server is overloaded. It is a single non-blocking write
// so a slow client cannot stall the accepting thread.
void rejectBusy(int fd)
{
    Response response = makeErrorResponse(Status::ServiceUnavailable);
    response.headers.push_back({"Retry-After", "1"});
    response.headers.push_back({"Connection", "close"});
    const std::string message = serializeHead(response) + std::get<std::string>(response.body);
    static_cast<void>(::send(fd, message.data(), message.size(), MSG_DONTWAIT | MSG_NOSIGNAL));
}

} // namespace

Server::Server(ServerConfig config, Handler handler)
    : config_{std::move(config)}
    , handler_{std::move(handler)}
    , listener_{TcpListener::bind(config_.address, config_.port)}
    , stop_{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)}
{
    if (!stop_) {
        detail::throwSystemError("eventfd");
    }
}

std::uint16_t Server::port() const
{
    return listener_.port();
}

void Server::run()
{
    // Destroyed when run() returns, which waits for the workers. They see
    // the stop request too, so idle connections close straight away.
    ThreadPool pool{config_.threadCount, config_.queueCapacity};

    for (;;) {
        std::array<pollfd, 2> fds{{{listener_.fd(), POLLIN, 0}, {stop_.get(), POLLIN, 0}}};
        if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            detail::throwSystemError("poll");
        }
        if (fds[1].revents != 0) {
            return;
        }

        auto connection = listener_.accept();
        if (!connection) {
            if (isResourceAcceptError(connection.error())) {
                std::osyncstream{std::cerr}
                    << "http-server: accept: " << connection.error().message() << '\n';
                std::this_thread::sleep_for(100ms);
            } else if (!isTransientAcceptError(connection.error())) {
                throw std::system_error{connection.error(), "accept"};
            }
            continue;
        }

        const int fd = connection->get();
        ThreadPool::Task task = [this, connection = std::move(*connection)]() mutable {
            detail::serveConnection(std::move(connection), handler_, config_, stop_.get());
        };
        if (!pool.trySubmit(std::move(task))) {
            // The task still owns the connection, so fd is open here; it is
            // closed when the task goes out of scope.
            rejectBusy(fd);
        }
    }
}

void Server::requestStop() noexcept
{
    // write() is async-signal-safe. The only possible failure is the counter
    // overflowing, which still leaves the eventfd readable.
    const std::uint64_t one = 1;
    static_cast<void>(::write(stop_.get(), &one, sizeof one));
}

} // namespace http
