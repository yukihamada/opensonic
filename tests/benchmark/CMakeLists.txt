# Benchmark configuration for Soluna
# Enable with: cmake -B build -DSOLUNA_BUILD_BENCHMARKS=ON

include(FetchContent)

# Google Benchmark
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.3
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

# Latency benchmark
add_executable(bench_latency bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE soluna_core benchmark::benchmark benchmark::benchmark_main)

# Throughput benchmark
add_executable(bench_throughput bench_throughput.cpp)
target_link_libraries(bench_throughput PRIVATE soluna_core benchmark::benchmark benchmark::benchmark_main)

# Memory benchmark
add_executable(bench_memory bench_memory.cpp)
target_link_libraries(bench_memory PRIVATE soluna_core benchmark::benchmark benchmark::benchmark_main)
