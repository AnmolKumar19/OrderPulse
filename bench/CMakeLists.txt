find_package(Threads REQUIRED)

add_executable(orderpulse_bench
    bench_latency.cpp
    latency.cpp
)
target_link_libraries(orderpulse_bench PRIVATE orderpulse_core Threads::Threads)
target_compile_options(orderpulse_bench PRIVATE -Wall -Wextra -g)
