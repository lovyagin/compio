cmake_minimum_required(VERSION 3.10)
project(compio)

find_package(GTest REQUIRED)

add_executable(compio_gtest
    test_runner.cpp
    unit/test_allocator.cpp
    unit/test_allocator_comprehensive.cpp
    unit/test_allocator_advanced.cpp
    unit/test_allocator_edge_cases.cpp
    unit/test_allocator_serialization.cpp
    unit/test_library.cpp
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