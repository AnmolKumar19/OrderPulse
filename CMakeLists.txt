cmake_minimum_required(VERSION 3.16)
project(OrderPulse CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ORDERPULSE_BUILD_TESTS "Build the Catch2 correctness test suite" ON)
option(ORDERPULSE_BUILD_BENCH "Build the latency benchmark harness" ON)
option(ORDERPULSE_BUILD_SERVER "Build the web demo server" ON)

# orderpulse_core: the order book + matching engine, no threading and no I/O.
# Kept as its own static library so tests and the benchmark link the exact
# same compiled code (no header-only ODR surprises, no divergence between
# what's tested and what's measured).
add_library(orderpulse_core STATIC
    src/order_pool.cpp
    src/order_book.cpp
    src/feed_simulator.cpp
    src/price_ladder.cpp
)
target_include_directories(orderpulse_core PUBLIC include)
target_compile_options(orderpulse_core PRIVATE
    -Wall -Wextra -g
    $<$<CONFIG:Release>:-O3>
)

if(ORDERPULSE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ORDERPULSE_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(ORDERPULSE_BUILD_SERVER)
    add_subdirectory(web/server)
endif()
