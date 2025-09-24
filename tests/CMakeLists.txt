cmake_minimum_required(VERSION 3.10)
project(compio)

find_package(GTest REQUIRED)

add_executable(compio_gtest
    test_runner.cpp
    unit/test_allocator.cpp
    unit/test_library.cpp
)

target_compile_features(compio_gtest PRIVATE cxx_std_17)
target_link_libraries(compio_gtest PRIVATE compio GTest::GTest GTest::Main)
target_include_directories(compio_gtest PRIVATE 
    ${CMAKE_SOURCE_DIR} 
    ${CMAKE_SOURCE_DIR}/include
)

enable_testing()

add_test(NAME compio_gtest_tests COMMAND compio_gtest)