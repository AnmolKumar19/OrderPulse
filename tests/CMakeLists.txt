# Catch2 vendored as the amalgamated single header+source pair (see
# third_party/catch2/) — no network access needed at build time, no
# FetchContent, no system package dependency.
add_library(catch2_amalgamated STATIC
    ${CMAKE_SOURCE_DIR}/third_party/catch2/catch_amalgamated.cpp
)
target_include_directories(catch2_amalgamated PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/catch2
)

find_package(Threads REQUIRED)

add_executable(orderpulse_tests
    test_order_book.cpp
    test_spsc_queue.cpp
    test_concurrency.cpp
    test_feed_simulator.cpp
)
target_link_libraries(orderpulse_tests PRIVATE orderpulse_core catch2_amalgamated Threads::Threads)
target_compile_options(orderpulse_tests PRIVATE -Wall -Wextra)

include(CTest)
add_test(NAME orderpulse_tests COMMAND orderpulse_tests)
