cmake_minimum_required(VERSION 3.20)

project(cpp_http_server LANGUAGES CXX)

add_executable(http-server src/main.cpp)

set_target_properties(http-server PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

target_compile_options(http-server PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

include(CTest)

if(BUILD_TESTING)
    add_test(NAME startup COMMAND http-server)
    set_tests_properties(startup PROPERTIES
        PASS_REGULAR_EXPRESSION "HTTP server project ready"
    )
endif()
