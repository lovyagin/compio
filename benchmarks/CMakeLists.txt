cmake_minimum_required(VERSION 3.14)
project(compio_benchmarks)

find_package(benchmark REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(compio_benchmarks
    src/benchmarks_main.cpp
    src/consecutive_write.cpp
    src/random_write.cpp
    src/consecutive_read.cpp
    src/random_read.cpp
)

target_link_libraries(compio_benchmarks PRIVATE 
    benchmark::benchmark
    compio
)
target_include_directories(compio_benchmarks PRIVATE 
    ${CMAKE_SOURCE_DIR} 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

install(TARGETS compio_benchmarks DESTINATION bin)

add_executable(fsize_benchmark custom/fsize_benchmark.cpp)

target_link_libraries(fsize_benchmark PRIVATE compio)

target_include_directories(fsize_benchmark PRIVATE 
    ${CMAKE_SOURCE_DIR} 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(fragmentation custom/fragmentation.cpp)

target_link_libraries(fragmentation PRIVATE compio)

target_include_directories(fragmentation PRIVATE 
    ${CMAKE_SOURCE_DIR} 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(random_usage custom/random_usage.cpp)

target_link_libraries(random_usage PRIVATE compio)

target_include_directories(random_usage PRIVATE 
    ${CMAKE_SOURCE_DIR} 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)