cmake_minimum_required(VERSION 3.22)

project(fps_opto_pro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FPS_OPTO_PRO_BUILD_TESTS "Build the host tests" ON)

# The preloader SDK. The Android build links it when a checkout is provided;
# the host test build never needs it.
set(PRELOADER_ANDROID_ROOT "" CACHE PATH "Path to a LiteLDev/preloader-android checkout")

if(FPS_OPTO_PRO_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_subdirectory(tests)
    return()
endif()

if(NOT ANDROID)
    message(FATAL_ERROR
        "This project builds for Android. Configure the host build with "
        "-DFPS_OPTO_PRO_BUILD_TESTS=ON and no toolchain file to run the tests.")
endif()

if(NOT EXISTS "${PRELOADER_ANDROID_ROOT}/include/pl/Config.hpp")
    message(FATAL_ERROR
        "Set -DPRELOADER_ANDROID_ROOT to a preloader-android checkout. "
        "Missing: ${PRELOADER_ANDROID_ROOT}/include/pl/Config.hpp")
endif()

include(FetchContent)

set(FPS_OPTO_PRO_DEPS)
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    list(APPEND FPS_OPTO_PRO_DEPS nlohmann_json)
endif()

if(NOT TARGET Boost::pfr)
    FetchContent_Declare(
        boost_pfr
        GIT_REPOSITORY https://github.com/boostorg/pfr.git
        GIT_TAG boost-1.88.0)
    list(APPEND FPS_OPTO_PRO_DEPS boost_pfr)
endif()

if(NOT TARGET magic_enum::magic_enum)
    FetchContent_Declare(
        magic_enum
        GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
        GIT_TAG v0.9.7)
    list(APPEND FPS_OPTO_PRO_DEPS magic_enum)
endif()

if(NOT TARGET fmt::fmt)
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 11.2.0)
    set(FMT_DOC OFF CACHE BOOL "Disable fmt docs" FORCE)
    set(FMT_TEST OFF CACHE BOOL "Disable fmt tests" FORCE)
    set(FMT_INSTALL OFF CACHE BOOL "Disable fmt install rules" FORCE)
    list(APPEND FPS_OPTO_PRO_DEPS fmt)
endif()

if(FPS_OPTO_PRO_DEPS)
    FetchContent_MakeAvailable(${FPS_OPTO_PRO_DEPS})
endif()

function(fps_opto_pro_configure target_name)
    target_include_directories("${target_name}" PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/src"
        "${PRELOADER_ANDROID_ROOT}/include")
    target_link_libraries("${target_name}" PRIVATE
        nlohmann_json::nlohmann_json
        Boost::pfr
        magic_enum::magic_enum
        fmt::fmt)
    target_compile_options("${target_name}" PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall -Wextra -Wpedantic>)
endfunction()

option(FPS_OPTO_PRO_LINK_PRELOADER
       "Link the local preloader target instead of leaving symbols unresolved"
       ON)

if(FPS_OPTO_PRO_LINK_PRELOADER AND NOT TARGET preloader)
    add_subdirectory("${PRELOADER_ANDROID_ROOT}"
                     "${CMAKE_BINARY_DIR}/_preloader_android")
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/out/${ANDROID_ABI}")

find_library(FPS_OPTO_PRO_LOG_LIB log)
find_library(FPS_OPTO_PRO_GLES_LIB GLESv3)
find_library(FPS_OPTO_PRO_EGL_LIB EGL)

add_library(fps_opto_pro SHARED
    src/FpsOptoPro.cpp
    src/GlInterceptor.cpp
    src/EglSensor.cpp
    src/ThreadScheduler.cpp
    src/AdpfSession.cpp
    src/BootMarker.cpp
    src/Diagnostics.cpp)

set_target_properties(fps_opto_pro PROPERTIES OUTPUT_NAME "fps_opto_pro")
fps_opto_pro_configure(fps_opto_pro)

target_compile_options(fps_opto_pro PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fvisibility=hidden -fvisibility-inlines-hidden>)

foreach(lib FPS_OPTO_PRO_LOG_LIB FPS_OPTO_PRO_GLES_LIB FPS_OPTO_PRO_EGL_LIB)
    if(${lib})
        target_link_libraries(fps_opto_pro PRIVATE "${${lib}}")
    endif()
endforeach()

# The launcher package ships arm64-v8a only, so an armeabi-v7a build is a
# configuration error rather than a supported target.
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(WARNING
        "ChimeraLauncher ships arm64-v8a only; building for ${ANDROID_ABI}")
endif()

if(TARGET preloader)
    target_link_libraries(fps_opto_pro PRIVATE preloader)
else()
    target_link_options(fps_opto_pro PRIVATE
        -Wl,--unresolved-symbols=ignore-all)
endif()