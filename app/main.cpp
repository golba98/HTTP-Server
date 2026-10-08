#include "http/Server.hpp"
#include "http/StaticFiles.hpp"

#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::string_view usage = R"(Usage: http-server [options]

Serves the files in a directory over HTTP/1.1.

Options:
  --address ADDR   IPv4 address to listen on (default 127.0.0.1)
  --port PORT      TCP port; 0 picks a free one (default 8080)
  --root DIR       directory to serve (default public)
  --threads N      worker threads (default 64)
  --quiet          do not log requests
  --version        print the version and exit
  --help           print this help and exit
)";

// Exit statuses besides 0.
constexpr int runtimeFailure = 1;
constexpr int usageFailure = 2;

struct Options {
    http::ServerConfig config;
    std::filesystem::path root{"public"};
};

template <typename Number> std::optional<Number> parseNumber(std::string_view text)
{
    Number value{};
    const char* const end = std::to_address(text.end());
    const auto [parsedTo, error] = std::from_chars(std::to_address(text.begin()), end, value);
    if (text.empty() || error != std::errc{} || parsedTo != end) {
        return std::nullopt;
    }
    return value;
}

std::unexpected<int> usageError(std::string_view message)
{
    std::cerr << "http-server: " << message << "\n\n" << usage;
    return std::unexpected{usageFailure};
}

// Returns the options, or the exit status if the program should stop now.
std::expected<Options, int> parseArguments(std::span<char* const> args)
{
    Options options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help") {
            std::cout << usage;
            return std::unexpected{0};
        }
        if (arg == "--version") {
            std::cout << "http-server " << HTTP_SERVER_VERSION << '\n';
            return std::unexpected{0};
        }
        if (arg == "--quiet") {
            options.config.accessLog = false;
            continue;
        }
        if (arg != "--address" && arg != "--port" && arg != "--root" && arg != "--threads") {
            return usageError("unknown option '" + std::string{arg} + "'");
        }
        if (i + 1 == args.size()) {
            return usageError("missing value for " + std::string{arg});
        }
        const std::string_view value = args[++i];

        if (arg == "--address") {
            options.config.address = value;
        } else if (arg == "--root") {
            options.root = value;
        } else if (arg == "--port") {
            const auto port = parseNumber<std::uint16_t>(value);
            if (!port) {
                return usageError("invalid port '" + std::string{value} + "'");
            }
            options.config.port = *port;
        } else {
            const auto threads = parseNumber<std::size_t>(value);
            if (!threads || *threads == 0) {
                return usageError("invalid thread count '" + std::string{value} + "'");
            }
            options.config.threadCount = *threads;
        }
    }
    return options;
}

sigset_t shutdownSignals()
{
    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

// Waits for SIGINT or SIGTERM and asks the server to stop. It wakes up
// regularly to check `stop`, so it can be joined even if the server fails
// without a signal ever arriving.
void stopOnSignal(const std::stop_token& stop, const sigset_t& signals, http::Server& server)
{
    const timespec interval{0, 200'000'000};
    while (!stop.stop_requested()) {
        if (::sigtimedwait(&signals, nullptr, &interval) >= 0) {
            server.requestStop();
            return;
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    const auto options = parseArguments({argv, static_cast<std::size_t>(argc)});
    if (!options) {
        return options.error();
    }

    // Block the shutdown signals before any thread exists, so every thread
    // inherits the mask and they are only ever received by sigtimedwait()
    // below. That avoids asynchronous signal handlers altogether.
    const sigset_t signals = shutdownSignals();
    if (::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "http-server: cannot block signals\n";
        return runtimeFailure;
    }

    try {
        const http::StaticFileHandler files{options->root};
        http::Server server{options->config,
            [&files](const http::Request& request) { return files.handle(request); }};

        // Flushed so scripts reading the output see the port immediately.
        std::cout << "Listening on http://" << options->config.address << ':' << server.port()
                  << " (root: " << options->root.string() << ")\n"
                  << std::flush;

        const std::jthread signalWaiter{[&signals, &server](const std::stop_token& stop) {
            stopOnSignal(stop, signals, server);
        }};
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "http-server: " << error.what() << '\n';
        return runtimeFailure;
    }
    return 0;
}
