#include <iostream>
#include <span>
#include <string_view>

namespace {

constexpr std::string_view usage = "Usage: http-server [--help] [--version]\n";

} // namespace

int main(int argc, char* argv[])
{
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};

    for (const std::string_view arg : args.subspan(1)) {
        if (arg == "--version") {
            std::cout << "http-server " << HTTP_SERVER_VERSION << '\n';
            return 0;
        }
        if (arg == "--help") {
            std::cout << usage;
            return 0;
        }
        std::cerr << "http-server: unknown option '" << arg << "'\n" << usage;
        return 2;
    }

    std::cout << "HTTP server project ready\n";
    return 0;
}
