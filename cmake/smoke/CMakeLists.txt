# A standalone downstream project that consumes SciLex purely through
# find_package — the canonical proof that the installed config package (and its
# transitive REAL dependency) works. CI configures this with
# -DCMAKE_PREFIX_PATH=<install prefix> after installing REAL and SciLex, builds
# it, and runs the resulting executable through ctest.
cmake_minimum_required(VERSION 3.16)
project(scilex_smoke LANGUAGES CXX)

find_package(scilex CONFIG REQUIRED)

add_executable(smoke main.cpp)
target_link_libraries(smoke PRIVATE scilex::scilex)
set_target_properties(smoke PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

enable_testing()
add_test(NAME smoke COMMAND smoke)
