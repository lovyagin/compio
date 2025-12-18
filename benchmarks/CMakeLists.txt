find_package(benchmark REQUIRED)

add_executable(compio_benchmarks
    src/benchmark_util.cpp
    src/benchmarks_main.cpp
    src/consecutive_read.cpp
    src/consecutive_write.cpp
    src/random_read.cpp
    src/random_write.cpp
    src/allocator_benchmark.cpp
)
target_link_libraries(compio_benchmarks PRIVATE compio benchmark::benchmark)
target_include_directories(compio_benchmarks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(fsize_benchmark custom/fsize_benchmark.cpp)
target_link_libraries(fsize_benchmark PRIVATE compio)
target_include_directories(fsize_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(blocks_benchmark custom/blocks_benchmark.cpp)
target_link_libraries(blocks_benchmark PRIVATE compio)
target_include_directories(blocks_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(random_usage custom/random_usage.cpp)
target_link_libraries(random_usage PRIVATE compio)
target_include_directories(random_usage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(fragmentation_benchmark custom/fragmentation_benchmark.cpp)
target_link_libraries(fragmentation_benchmark PRIVATE compio)
target_include_directories(fragmentation_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(compression_drift_benchmark custom/compression_drift_benchmark.cpp)
target_link_libraries(compression_drift_benchmark PRIVATE compio)
target_include_directories(compression_drift_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
