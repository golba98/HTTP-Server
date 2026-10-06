# C++ HTTP Server

A learning project to build an HTTP/1.1 server from scratch using C++20 and
Linux/POSIX socket APIs. The project will explore networking, HTTP parsing,
resource ownership, concurrency, and testing without an HTTP server framework.

## Current milestone

Stage 1 provides a minimal executable and a CTest smoke test. The executable
prints `HTTP server project ready` and exits. Networking and HTTP support will
be added incrementally.

## Prerequisites

- Linux (development environment: Fedora)
- GCC or Clang with C++20 support
- CMake 3.20 or newer
- Ninja

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

The startup test runs the executable and checks for its greeting.

## Project layout

```text
.
├── CMakeLists.txt
├── include/
│   └── http/       # Future public interfaces
├── src/
│   └── main.cpp
├── tests/         # Future protocol and integration tests
├── public/        # Future static website assets
├── README.md
└── .gitignore
```

The empty directories reserve space for later stages. Tests currently live in
the CMake configuration; no testing library is required yet.

## Build settings

The executable requires C++20 with compiler extensions disabled. Warning flags
are attached to the executable target rather than applied globally:

```text
-Wall -Wextra -Wpedantic -Wconversion -Wshadow
```

Warnings should be investigated and fixed. `-Werror` is not enabled initially.

## Next milestone

Stage 2 introduces move-only RAII ownership of socket file descriptors, then
builds a synchronous TCP server. Each stage will be implemented, reviewed,
built, and tested before moving forward.
# HTTP-Server
