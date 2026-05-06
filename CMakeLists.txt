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

add_executable(test_smoke tests/test_smoke.c)
target_link_libraries(test_smoke PRIVATE nxmt_core)
add_test(NAME test_smoke COMMAND test_smoke)
