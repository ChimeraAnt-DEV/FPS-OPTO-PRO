cmake_minimum_required(VERSION 3.22)

# Host tests. These build the same decision code the Android library compiles,
# with the GL entry points replaced by probes that count what actually reached
# the "driver". No mocks: the state machine under test is the production one.

add_executable(fps_opto_pro_tests
    test_main.cpp
    test_gl_filter.cpp
    test_governor.cpp
    test_telemetry.cpp
    test_policy.cpp
    ../src/GlInterceptor.cpp
    ../src/Diagnostics.cpp
    ../src/BootMarker.cpp)

target_include_directories(fps_opto_pro_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src")

target_compile_definitions(fps_opto_pro_tests PRIVATE
    PL_INTERPOSER_HOST_TEST=1)

target_compile_options(fps_opto_pro_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall -Wextra -Wpedantic>)

add_test(NAME fps_opto_pro_tests COMMAND fps_opto_pro_tests)