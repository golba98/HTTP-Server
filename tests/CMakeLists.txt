include(FetchContent)

# SYSTEM keeps the project's warning flags from applying to Catch2's headers.
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(Catch2)

add_executable(http_core_tests
    FileDescriptorTests.cpp
)
set_target_properties(http_core_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(http_core_tests PRIVATE
    http_core
    http_server_warnings
    Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(http_core_tests)

# End-to-end check that the executable links against http_core and runs.
add_test(NAME startup COMMAND http-server)
set_tests_properties(startup PROPERTIES
    PASS_REGULAR_EXPRESSION "HTTP server project ready"
)
