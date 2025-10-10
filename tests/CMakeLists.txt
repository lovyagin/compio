cmake_minimum_required(VERSION 3.10)
project(compio)

find_package(GTest REQUIRED)

add_executable(compio_gtest
    test_runner.cpp
    src/test_allocator.cpp
    src/test_allocator_comprehensive.cpp
    src/test_allocator_advanced.cpp
    src/test_allocator_edge_cases.cpp
    src/test_allocator_serialization.cpp
    src/test_compression_type.cpp
    src/test_library.cpp
)

target_compile_features(compio_gtest PRIVATE cxx_std_17)
target_link_libraries(compio_gtest PRIVATE compio GTest::GTest GTest::Main)
target_include_directories(compio_gtest PRIVATE 
    ${PROJECT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/../include
    ${PROJECT_SOURCE_DIR}/include
)

enable_testing()

add_test(NAME compio_gtest_tests COMMAND compio_gtest)