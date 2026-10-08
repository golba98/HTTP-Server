include(FetchContent)

# SYSTEM keeps the project's warning flags from applying to Catch2's headers.
# FIND_PACKAGE_ARGS uses an installed Catch2 3 when there is one, so builds can
# work offline; otherwise CMake downloads the pinned tag.
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
    SYSTEM
    FIND_PACKAGE_ARGS 3
)
FetchContent_MakeAvailable(Catch2)

add_executable(http_core_tests
    FileDescriptorTests.cpp
    MessageTests.cpp
    PathTests.cpp
    RequestParserTests.cpp
    SocketTests.cpp
    StaticFileTests.cpp
    ThreadPoolTests.cpp
)
set_target_properties(http_core_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(http_core_tests PRIVATE
    http_core
    http_server_warnings
    Catch2::Catch2WithMain
)

# The Catch module lives in the source tree when fetched, and next to the
# package config when installed.
if(catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
else()
    list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
endif()
include(Catch)
catch_discover_tests(http_core_tests)

# Command-line checks on the real executable. They exit immediately, unlike a
# plain run of the server, which blocks until it is signalled.
add_test(NAME cli.version COMMAND http-server --version)
set_tests_properties(cli.version PROPERTIES
    PASS_REGULAR_EXPRESSION "^http-server [0-9]+\\.[0-9]+\\.[0-9]+\n$"
)
add_test(NAME cli.help COMMAND http-server --help)
set_tests_properties(cli.help PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage: http-server"
)
add_test(NAME cli.bad-argument COMMAND http-server --no-such-option)
set_tests_properties(cli.bad-argument PROPERTIES WILL_FAIL TRUE)
