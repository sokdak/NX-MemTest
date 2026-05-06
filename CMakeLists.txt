cmake_minimum_required(VERSION 3.20)
project(NXMemTestHost C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_library(nxmt_core
    source/core/patterns.c
)
target_include_directories(nxmt_core PUBLIC include)
target_sources(nxmt_core PRIVATE source/core/arena.c)

add_executable(test_smoke tests/test_smoke.c)
target_link_libraries(test_smoke PRIVATE nxmt_core)
add_test(NAME test_smoke COMMAND test_smoke)

add_executable(test_patterns tests/test_patterns.c)
target_link_libraries(test_patterns PRIVATE nxmt_core)
add_test(NAME test_patterns COMMAND test_patterns)

add_executable(test_arena tests/test_arena.c)
target_link_libraries(test_arena PRIVATE nxmt_core)
add_test(NAME test_arena COMMAND test_arena)
