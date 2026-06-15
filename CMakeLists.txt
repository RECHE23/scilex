# Build system for SciLex. Single source of truth for compilation: warning
# policy, the sanitizer/coverage build variants, and the dependency on REAL.
# The root Makefile is a thin orchestrator over these targets.
#
# SciLex is header-only and depends on REAL's headers. By default it looks for
# them in a sibling checkout (../real-v1/include); override with
#   cmake -DSCILEX_REAL_INCLUDE=/path/to/real/include ...
# (e.g. the path printed by `python -c "import real; print(real.get_include())"`).
cmake_minimum_required(VERSION 3.16)
project(scilex LANGUAGES CXX)

# Use ccache as a compiler launcher when available (a no-op otherwise): the
# coverage / sanitizer / dual-compiler gates rebuild from scratch, so caching
# object files makes repeat runs near-instant. Override with -DUSE_CCACHE=OFF.
option(USE_CCACHE "Use ccache as the compiler launcher when found" ON)
if(USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endif()
endif()

option(SCILEX_SANITIZE "Build the tests with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(SCILEX_COVERAGE "Build the tests with LLVM source-based coverage instrumentation" OFF)

# Dependency resolution (REAL's headers), two modes:
#  * Default (local development): a sibling checkout, used live (uncommitted
#    cross-repo edits are visible). Override with -DSCILEX_REAL_INCLUDE=/path.
#  * CI / reproducible builds: -DSCILEX_FETCH_DEPS=ON fetches REAL with
#    FetchContent from a configurable repository + tag (no on-disk-layout
#    assumption). The repository defaults to the sibling git checkout, so it
#    works locally too; on CI point it at a remote and pin a tag, e.g.
#      -DSCILEX_FETCH_DEPS=ON -DSCILEX_REAL_REPO=https://… -DSCILEX_REAL_TAG=v2026.6.5
option(SCILEX_FETCH_DEPS "Fetch the dependencies with FetchContent" OFF)

set(SCILEX_REAL_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../real-v1/include"
    CACHE PATH "Path to REAL's include directory (when not fetching)")
set(SCILEX_REAL_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../real-v1"
    CACHE STRING "REAL git repository (path or URL) when fetching")
set(SCILEX_REAL_TAG "main" CACHE STRING "REAL git tag when fetching")

if(SCILEX_FETCH_DEPS)
    include(FetchContent)
    FetchContent_Declare(real GIT_REPOSITORY "${SCILEX_REAL_REPO}" GIT_TAG "${SCILEX_REAL_TAG}")
    FetchContent_GetProperties(real)
    if(NOT real_POPULATED)
        FetchContent_Populate(real) # source only (header-only): no transitive CMake
    endif()
    set(SCILEX_REAL_INCLUDE "${real_SOURCE_DIR}/include")
endif()

if(NOT EXISTS "${SCILEX_REAL_INCLUDE}/real/real.hpp")
    message(FATAL_ERROR
        "REAL headers not found at SCILEX_REAL_INCLUDE='${SCILEX_REAL_INCLUDE}'. "
        "Use -DSCILEX_FETCH_DEPS=ON, or set -DSCILEX_REAL_INCLUDE=/path/to/real/include.")
endif()

# Header-only library: consumers link scilex::scilex to get both include paths.
add_library(scilex INTERFACE)
add_library(scilex::scilex ALIAS scilex)
target_include_directories(scilex INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(scilex SYSTEM INTERFACE "${SCILEX_REAL_INCLUDE}")
target_compile_features(scilex INTERFACE cxx_std_20)

include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    return()
endif()

enable_testing()

file(GLOB SCILEX_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(scilex_tests_bin ${SCILEX_TEST_SOURCES})
target_link_libraries(scilex_tests_bin PRIVATE scilex::scilex)
set_target_properties(scilex_tests_bin PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# Warnings-as-errors everywhere. AppleClang accepts the Clang flag set.
if(MSVC)
    target_compile_options(scilex_tests_bin PRIVATE /W4 /WX /Zc:__cplusplus /EHsc /utf-8)
else()
    target_compile_options(scilex_tests_bin PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(SCILEX_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "SCILEX_SANITIZE requires a GCC/Clang-family compiler")
    endif()
    target_compile_options(scilex_tests_bin PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    target_link_options(scilex_tests_bin PRIVATE -fsanitize=address,undefined)
endif()

if(SCILEX_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SCILEX_COVERAGE requires a Clang-family compiler")
    endif()
    target_compile_options(scilex_tests_bin PRIVATE
        -fprofile-instr-generate -fcoverage-mapping -O0 -g)
    target_link_options(scilex_tests_bin PRIVATE -fprofile-instr-generate)
endif()

add_test(NAME run_tests COMMAND $<TARGET_FILE:scilex_tests_bin>)
