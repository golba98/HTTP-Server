# libFuzzer targets. Build them with the fuzz preset, then run one with, e.g.:
#
#   mkdir -p /tmp/corpus
#   ./build/fuzz/fuzz/request_parser_fuzzer -dict=fuzz/http.dict \
#       /tmp/corpus fuzz/corpus/request_parser
#
# libFuzzer writes new inputs to the first directory, so keep the checked-in
# seed corpus second.

foreach(fuzzer IN ITEMS request_parser path)
    set(target ${fuzzer}_fuzzer)
    add_executable(${target})
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(${target} PRIVATE http_core http_server_warnings)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)

    # Replays the seed corpus once as a regression test; -runs=0 stops
    # before any new inputs are generated.
    add_test(NAME fuzz.${fuzzer}.corpus
        COMMAND ${target} -runs=0 ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${fuzzer})
endforeach()

target_sources(request_parser_fuzzer PRIVATE RequestParserFuzzer.cpp)
target_sources(path_fuzzer PRIVATE PathFuzzer.cpp)
