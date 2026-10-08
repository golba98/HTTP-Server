cmake_minimum_required(VERSION 3.25)

project(cpp_http_server VERSION 0.1.0 LANGUAGES CXX)

option(HTTP_SERVER_ENABLE_SANITIZERS
    "Build project targets with AddressSanitizer and UndefinedBehaviorSanitizer (Debug only)"
    OFF
)
option(HTTP_SERVER_WARNINGS_AS_ERRORS
    "Treat compiler warnings in project targets as errors (used by CI)"
    OFF
)

# Warning flags for first-party targets. Linked PRIVATE so they never leak
# into fetched dependencies.
add_library(http_server_warnings INTERFACE)
target_compile_options(http_server_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wsign-conversion
    -Wold-style-cast
    $<$<BOOL:${HTTP_SERVER_WARNINGS_AS_ERRORS}>:-Werror>
)

# Sanitizer flags. Linked PUBLIC from http_core so every consumer of an
# instrumented library is also compiled and linked with the sanitizer runtime.
add_library(http_server_sanitizers INTERFACE)

if(HTTP_SERVER_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR
            "HTTP_SERVER_ENABLE_SANITIZERS requires GCC or Clang "
            "(found ${CMAKE_CXX_COMPILER_ID})")
    endif()

    get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NOT is_multi_config AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(WARNING
            "HTTP_SERVER_ENABLE_SANITIZERS only applies to Debug builds; "
            "sanitizers are disabled for build type '${CMAKE_BUILD_TYPE}'")
    endif()

    # -fno-sanitize-recover makes UBSan abort on the first error so tests fail
    # instead of printing a report and carrying on.
    target_compile_options(http_server_sanitizers INTERFACE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
        $<$<CONFIG:Debug>:-fno-sanitize-recover=undefined>
        $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    )
    target_link_options(http_server_sanitizers INTERFACE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
endif()

add_library(http_core STATIC
    src/FileDescriptor.cpp
    src/Message.cpp
    src/Path.cpp
    src/RequestParser.cpp
    src/Socket.cpp
    src/StaticFiles.cpp
    src/Status.cpp
    src/ThreadPool.cpp
)
target_include_directories(http_core
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_compile_features(http_core PUBLIC cxx_std_23)
set_target_properties(http_core PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(http_core
    PUBLIC http_server_sanitizers
    PRIVATE http_server_warnings
)

add_executable(http-server app/main.cpp)
set_target_properties(http-server PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(http-server PRIVATE http_core http_server_warnings)
target_compile_definitions(http-server PRIVATE HTTP_SERVER_VERSION="${PROJECT_VERSION}")

include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
