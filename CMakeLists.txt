# Build system for SciLex. Single source of truth for compilation: warning
# policy, the sanitizer/coverage build variants, and the dependency on REAL.
# The root Makefile is a thin orchestrator over these targets.
#
# SciLex is header-only and depends on REAL's headers. By default it looks for
# them in a sibling checkout (the maintainer's local layout uses ../real-regex);
# override with cmake -DSCILEX_REAL_INCLUDE=...
# (e.g. the path printed by `python -c "import real; print(real.get_include())"`).
cmake_minimum_required(VERSION 3.16)

# Single source of truth for the version: pyproject.toml (which `make release`
# bumps and setup.py reads). Deriving it here means the CMake package version can
# never drift from the Python package — the defect that had left this frozen at
# 2026.6.1 while the wheels shipped 2026.6.3.
file(READ "${CMAKE_CURRENT_LIST_DIR}/pyproject.toml" _scilex_pyproject)
if(NOT _scilex_pyproject MATCHES "[\r\n]version = \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
  message(FATAL_ERROR "SciLex: could not parse the project version from pyproject.toml")
endif()
project(scilex VERSION "${CMAKE_MATCH_1}" LANGUAGES CXX)

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
#      -DSCILEX_FETCH_DEPS=ON -DSCILEX_REAL_REPO=https://… -DSCILEX_REAL_TAG=v2026.6.14
option(SCILEX_FETCH_DEPS "Fetch the dependencies with FetchContent" OFF)

set(SCILEX_REAL_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../real-regex/include"
    CACHE PATH "Path to REAL's include directory (maintainer's default; override for your layout)")
set(SCILEX_REAL_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../real-regex"
    CACHE STRING "REAL git repository (path or URL) when fetching")
# Default to REAL's released tag so a FetchContent build is reproducible (override
# with -DSCILEX_REAL_TAG=main to track the latest during co-development).
set(SCILEX_REAL_TAG "v2026.6.14" CACHE STRING "REAL git tag when fetching")

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
# Build tree: REAL's headers come from the resolved include path above. Install
# tree: a downstream find_package(scilex) instead links real::real, resolved by
# find_dependency(real) in scilexConfig.cmake — so the absolute build-tree path
# never leaks into the exported package.
target_include_directories(scilex SYSTEM INTERFACE $<BUILD_INTERFACE:${SCILEX_REAL_INCLUDE}>)
target_link_libraries(scilex INTERFACE $<INSTALL_INTERFACE:real::real>)
target_compile_features(scilex INTERFACE cxx_std_20)

include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# find_package(scilex CONFIG) support: export the interface target and generate a
# config package. The config calls find_dependency(real), so a downstream
# project gets the whole chain from a single find_package:
#   find_package(scilex CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE scilex::scilex)
install(TARGETS scilex EXPORT scilexTargets)
install(EXPORT scilexTargets
    NAMESPACE scilex::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/scilex)
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/scilexConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/scilexConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/scilex)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/scilexConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/scilexConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/scilexConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/scilex)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    return()
endif()

enable_testing()

# The test harness (framework.hpp) is owned by SciForge — the ecosystem's shared
# infrastructure repository — and is never carried as a local copy. Resolution
# mirrors how this project finds its other sibling dependencies, two modes:
#  * Default (local development): a sibling checkout, used live. The maintainer's
#    layout uses ../sciforge; override with -DSCIFORGE_INCLUDE_DIR=/path.
#  * CI / reproducible builds: -DSCILEX_FETCH_SCIFORGE=ON fetches SciForge with
#    FetchContent from a configurable repository + tag. Prepared but inert by
#    default; once SciForge publishes a CalVer tag, pin SCIFORGE_TAG to it.
option(SCILEX_FETCH_SCIFORGE "Fetch SciForge (test harness) with FetchContent" OFF)
set(SCIFORGE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sciforge/include"
    CACHE PATH "Path to SciForge's include directory (maintainer's default; override for your layout)")
set(SCIFORGE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../sciforge"
    CACHE STRING "SciForge git repository (path or URL) when fetching")
set(SCIFORGE_TAG "v2026.6.2" CACHE STRING "SciForge git tag when fetching (pin to a CalVer tag once released)")

if(SCILEX_FETCH_SCIFORGE)
    include(FetchContent)
    FetchContent_Declare(sciforge GIT_REPOSITORY "${SCIFORGE_REPO}" GIT_TAG "${SCIFORGE_TAG}")
    FetchContent_GetProperties(sciforge)
    if(NOT sciforge_POPULATED)
        FetchContent_Populate(sciforge) # source only (header-only): no transitive CMake
    endif()
    set(SCIFORGE_INCLUDE_DIR "${sciforge_SOURCE_DIR}/include")
endif()

if(NOT EXISTS "${SCIFORGE_INCLUDE_DIR}/sciforge/test/framework.hpp")
    message(FATAL_ERROR
        "SciForge test harness not found at SCIFORGE_INCLUDE_DIR='${SCIFORGE_INCLUDE_DIR}'. "
        "Use -DSCILEX_FETCH_SCIFORGE=ON, or set -DSCIFORGE_INCLUDE_DIR=/path/to/sciforge/include.")
endif()

file(GLOB SCILEX_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(scilex_tests_bin ${SCILEX_TEST_SOURCES})
target_link_libraries(scilex_tests_bin PRIVATE scilex::scilex)
# The harness include path comes from SciForge, not from a local tests/ copy.
target_include_directories(scilex_tests_bin PRIVATE "${SCIFORGE_INCLUDE_DIR}")
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
