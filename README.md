# C++ HTTP Server

A learning project to build an HTTP/1.1 server from scratch using C++20 and
Linux/POSIX socket APIs. The project will explore networking, HTTP parsing,
resource ownership, concurrency, and testing without an HTTP server framework.

## Current milestone

Stage 1 provides a minimal executable and a CTest smoke test. The executable
prints `HTTP server project ready` and exits. Networking and HTTP support will
be added incrementally.

Stage 2 has started: reusable code now lives in the `http_core` static library,
beginning with `http::FileDescriptor`, a move-only RAII owner of a POSIX file
descriptor, covered by Catch2 unit tests.

## Prerequisites

- Linux (development environment: Fedora)
- GCC or Clang with C++20 support
- CMake 3.25 or newer
- Ninja
- Network access the first time you configure, so CMake can download
  [Catch2](https://github.com/catchorg/Catch2) v3 (it is not needed globally)

## Configure and build

Run these commands from the project root:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++
cmake --build build
```

CMake generates the build instructions, and Ninja executes them. Generated
files stay in `build/`, separate from the source files.

## Run

```bash
./build/http-server
```

Expected output:

```text
HTTP server project ready
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

Unit tests use Catch2 v3, fetched by CMake's `FetchContent`. Each Catch2 test
case is registered with CTest individually. The `startup` test additionally
runs the executable and checks for its greeting.

## Project layout

```text
.
├── CMakeLists.txt
├── include/
│   └── http/      # Public headers of the http_core library
├── src/           # http_core library sources
├── app/
│   └── main.cpp   # Thin executable entry point
├── tests/         # Catch2 unit tests and CTest registration
├── public/        # Future static website assets
├── README.md
└── .gitignore
```

Production code is built once into the `http_core` static library. Both the
`http-server` executable and the `http_core_tests` test executable link
against it.

## Build settings

Project targets require C++20 with compiler extensions disabled. Warning flags
are attached to the project's own targets rather than applied globally, so
they do not affect fetched dependencies:

```text
-Wall -Wextra -Wpedantic -Wconversion -Wshadow
```

Warnings should be investigated and fixed. `-Werror` is not enabled initially.

### Sanitizers

AddressSanitizer and UndefinedBehaviorSanitizer can be enabled for Debug
builds. They are off by default and are never applied to other build types:

```bash
cmake -S . -B build-sanitize -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHTTP_SERVER_ENABLE_SANITIZERS=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```

UBSan is configured to abort on the first error so that tests fail. With GCC
on Fedora, the runtimes come from the `libasan` and `libubsan` packages.

## Next milestone

Stage 2 introduces move-only RAII ownership of socket file descriptors, then
builds a synchronous TCP server. Each stage will be implemented, reviewed,
built, and tested before moving forward.
