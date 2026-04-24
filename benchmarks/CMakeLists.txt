find_package(benchmark REQUIRED)

add_executable(compio_benchmarks
    src/benchmark_util.cpp
    src/benchmarks_main.cpp
    src/consecutive_read.cpp
    src/consecutive_write.cpp
    src/random_read.cpp
    src/random_write.cpp
    src/optimal_usage.cpp
    src/allocator_benchmark.cpp
    src/defragmentation_benchmark.cpp
    src/lookup_benchmark.cpp
    src/checksum_benchmark.cpp
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

add_executable(index_benchmark_custom src/index_benchmark_custom.cpp)
target_link_libraries(index_benchmark_custom PRIVATE compio)

add_executable(io_overhead custom/io_overhead.cpp)
target_link_libraries(io_overhead PRIVATE compio)

add_executable(size_comparison_3way
    custom/size_comparison_3way.cpp
    third_party/zstd_seekable/zstdseek_compress.c
)
target_include_directories(size_comparison_3way PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd_seekable
)
target_link_libraries(size_comparison_3way PRIVATE
    compio
    ZLIB::ZLIB
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
)

add_executable(locality_access_comparison
    custom/locality_access_comparison.cpp
    third_party/zran/zran.c
    third_party/zstd_seekable/zstdseek_compress.c
    third_party/zstd_seekable/zstdseek_decompress.c
)
target_include_directories(locality_access_comparison PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zran
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd_seekable
)
target_link_libraries(locality_access_comparison PRIVATE
    compio
    ZLIB::ZLIB
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
)
target_compile_definitions(locality_access_comparison PRIVATE
    _POSIX_C_SOURCE=200809L
    _FILE_OFFSET_BITS=64
)
