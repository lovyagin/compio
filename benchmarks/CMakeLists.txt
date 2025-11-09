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

add_executable(fragmentation custom/fragmentation.cpp)
target_link_libraries(fragmentation PRIVATE compio)
target_include_directories(fragmentation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(random_usage custom/random_usage.cpp)
target_link_libraries(random_usage PRIVATE compio)
target_include_directories(random_usage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(advanced_fragmentation custom/advanced_fragmentation.cpp)
target_link_libraries(advanced_fragmentation PRIVATE compio)
target_include_directories(advanced_fragmentation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
