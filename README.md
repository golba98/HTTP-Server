# C++ HTTP Server

A learning project to build an HTTP/1.1 server from scratch using C++23 and
Linux/POSIX socket APIs. The project explores networking, HTTP parsing,
resource ownership, concurrency, and testing without an HTTP server framework.

## Current milestone

The server serves static files from a directory over HTTP/1.1:

- **Sockets**: RAII ownership of every descriptor (`http::FileDescriptor`),
  close-on-exec everywhere, and I/O that never raises `SIGPIPE`.
- **Parsing**: an incremental, I/O-free request parser that follows RFC 9112,
  with limits on the request line (414), header section (431) and body (413),
  and with `Content-Length` and `chunked` bodies. It rejects ambiguous
  framing that enables request smuggling.
- **Static files**: `GET` and `HEAD`, `index.html` for directories,
  `Content-Type` by extension, and lookups confined to the document root by
  the kernel (`openat2` with `RESOLVE_BENEATH`).
- **Connections**: keep-alive, pipelining, idle and request timeouts (408), and
  a bounded worker pool that answers 503 when it is full.
- **Operations**: an access log, and a clean shutdown on `SIGINT`/`SIGTERM`.

It is tested with Catch2 unit and integration tests, an end-to-end smoke test,
AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer, and libFuzzer.

## Prerequisites

- Linux 5.6 or newer (for `openat2`); development environment: Fedora
- GCC or Clang with C++23 support (GCC 14+, Clang 18+)
- CMake 3.25 or newer
- Ninja
- Network access the first time you configure, so CMake can download
  [Catch2](https://github.com/catchorg/Catch2) v3, unless Catch2 3 is
  already installed
- Optional:
  - Clang and its compiler-rt runtimes for ThreadSanitizer and fuzzing
  - `curl` for the smoke test
  - clang-format and clang-tidy for linting

## Configure and build

The project uses CMake presets. Each preset builds into `build/<preset>/`:

```bash
cmake --preset debug
cmake --build --preset debug
```

| Preset     | What it builds                                             |
| ---------- | ---------------------------------------------------------- |
| `debug`    | Debug build with the default compiler                      |
| `sanitize` | Debug build with AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan`     | Debug build with ThreadSanitizer (uses `clang++`)           |
| `fuzz`     | libFuzzer targets with ASan + UBSan (uses `clang++`)        |
| `release`  | Optimized build                                            |

To use another compiler, add `-DCMAKE_CXX_COMPILER=clang++` (or similar) to
the configure command. Every preset exports `compile_commands.json`, and
`.clangd` points clangd at the debug one.

## Run

```bash
./build/debug/http-server
```

```text
Listening on http://127.0.0.1:8080 (root: public)
```

Then visit <http://127.0.0.1:8080/>, or:

```bash
curl -i http://127.0.0.1:8080/
curl -I http://127.0.0.1:8080/style.css
```

Press Ctrl-C to stop the server; it finishes the requests in progress and
exits with status 0. Options:

```text
--address ADDR   IPv4 address to listen on (default 127.0.0.1)
--port PORT      TCP port; 0 picks a free one (default 8080)
--root DIR       directory to serve (default public)
--threads N      worker threads (default 64)
--quiet          do not log requests
--version        print the version and exit
--help           print this help and exit
```

Each request is logged to stderr:

```text
127.0.0.1:44116 "GET /style.css HTTP/1.1" 200 230
```

## Test

```bash
ctest --preset debug
```

The tests are:

- Catch2 unit tests for each component: descriptors, sockets, messages,
  parser, paths, static files and thread pool.
- In-process server tests, which start a `Server` on a free port and talk to
  it over real sockets.
- `cli.*` tests of the command-line interface.
- `smoke`, which runs the real executable, fetches a page with `curl`, and
  checks that `SIGTERM` stops it cleanly.

Parser tests run every input twice: all at once, and one byte at a time as the
slowest possible client would send it. Both must give the same result.

## Sanitizers

```bash
cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

`sanitize` enables AddressSanitizer and UndefinedBehaviorSanitizer; UBSan
aborts on the first error so tests fail. `tsan` enables ThreadSanitizer, which
cannot be combined with ASan. Configuring both options at once is an error.
Sanitizers only apply to Debug builds.

With GCC on Fedora, the ASan and UBSan runtimes come from the `libasan` and
`libubsan` packages, and TSan needs `libtsan`. The `tsan` preset uses Clang,
whose runtimes ship with `compiler-rt`.

## Fuzzing

```bash
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p /tmp/corpus
./build/fuzz/fuzz/request_parser_fuzzer -dict=fuzz/http.dict /tmp/corpus fuzz/corpus/request_parser
./build/fuzz/fuzz/path_fuzzer -dict=fuzz/http.dict /tmp/corpus-path fuzz/corpus/path
```

The fuzzers do more than look for crashes:

- `request_parser_fuzzer` checks that one-shot and byte-by-byte parsing agree.
- `path_fuzzer` checks that an accepted path can never leave the document
  root.

libFuzzer writes new inputs to the first directory, so keep the checked-in
seeds second. `ctest --preset fuzz` replays the seed corpora as regression
tests.

## Lint and format

```bash
clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.hpp')
clang-tidy -p build/debug src/*.cpp app/*.cpp
```

`.clang-format` matches the existing style. `.clang-tidy` enables the
`bugprone`, `cppcoreguidelines`, `modernize` and `performance` checks, and
lists a reason for each check it disables.

## Continuous integration

`.github/workflows/ci.yml` runs on pushes to `main` and on pull requests:

- `debug` and `sanitize` builds with GCC 14 and Clang 18
- `tsan`
- `fuzz`, with one minute of fuzzing per target
- clang-format and clang-tidy checks

CI builds with `HTTP_SERVER_WARNINGS_AS_ERRORS=ON`.

## Project layout

```text
.
├── CMakeLists.txt
├── CMakePresets.json
├── include/http/      # Public headers of the http_core library
├── src/               # http_core sources and internal headers
├── app/main.cpp       # Command-line entry point
├── tests/             # Catch2 tests, test helpers and the smoke test
├── fuzz/              # libFuzzer targets, dictionary and seed corpora
├── public/            # Sample site served by default
└── .github/workflows/ # CI
```

Production code is built once into the `http_core` static library. The
`http-server` executable, the test executable and the fuzzers all link against
it.

## Build settings

Project targets require C++23 with compiler extensions disabled. Warning flags
are attached to the project's own targets rather than applied globally, so
they do not affect fetched dependencies:

```text
-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wsign-conversion -Wold-style-cast
```

`-DHTTP_SERVER_WARNINGS_AS_ERRORS=ON` adds `-Werror`; CI turns it on.

## Design notes

- **The parser does no I/O.** `RequestParser` is a line-oriented state machine.
  The connection code appends whatever arrives to a buffer and passes the
  whole buffer each time. The parser remembers where it stopped, so a client
  trickling one byte at a time cannot make it rescan the buffer, which would
  be O(n²). It is tested and fuzzed without any sockets.
- **Ambiguous requests are rejected, not repaired.** These all get an error
  instead of a guess: bare LF line endings, `Transfer-Encoding` together with
  `Content-Length`, repeated `Content-Length`, and a missing or duplicate
  `Host`. A proxy in front of the server could guess differently, and that
  disagreement is how request smuggling works.
- **File bodies use `pread` + `send`, not `sendfile`.** `sendfile(2)` cannot
  suppress `SIGPIPE` for a single call, so a client that disconnects mid-download
  would kill the process unless the signal were ignored process-wide.
  `send(MSG_NOSIGNAL)` keeps the guarantee local.
- **The kernel enforces the document root.** Paths are decoded, and any `..`
  segment is rejected. Files are then opened with `openat2(RESOLVE_BENEATH)`
  relative to the root, so a symlink cannot escape either. `O_NONBLOCK` keeps a
  FIFO from hanging a worker.
- **Concurrency is a fixed thread pool with blocking I/O.**
  - One thread accepts connections. Workers each serve one connection with
    simple sequential code.
  - The queue is bounded, and overflow gets an immediate 503.
  - Every wait is a `poll()` that also watches an `eventfd`. `requestStop()`
    writes to it, so idle keep-alive connections close at once on shutdown.
- **Timeouts bound how long a client can hold a worker.** An idle connection
  is closed after 5 s. A request that has started must be complete within
  10 s, otherwise the client gets 408. `SO_SNDTIMEO` bounds writes to a client
  that has stopped reading. Error responses are followed by a lingering close
  so the client reads them before the connection drops.
- **Signals are received, not handled.** `main` blocks `SIGINT` and `SIGTERM`
  before starting any thread, and a dedicated thread picks them up with
  `sigtimedwait()`. That avoids asynchronous signal handlers entirely.

## Possible next steps

- IPv6 and dual-stack listening
- Conditional requests (`ETag`, `If-Modified-Since`) and `Range` requests
- An `epoll`-based event loop, to compare with the thread pool
- TLS
