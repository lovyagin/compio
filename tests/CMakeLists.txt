cmake_minimum_required(VERSION 3.10)

find_package(GTest REQUIRED)

file(GLOB_RECURSE CUNIT_TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/*.c)

file(GLOB_RECURSE GTEST_TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/*.cpp)

add_executable(compio_cunit
        test_runner.c
        ${CUNIT_TEST_SOURCES}
)

add_executable(compio_gtest
        test_runner.cpp
        ${GTEST_TEST_SOURCES}
)

target_compile_definitions(compio_cunit PRIVATE CUNIT_DISABLE_TEST_SNPRINTF)
target_link_libraries(compio_cunit PRIVATE compio_lib unofficial::cunit::cunit)
target_include_directories(compio_cunit PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_compile_features(compio_gtest PRIVATE cxx_std_17)
target_link_libraries(compio_gtest PRIVATE compio_lib GTest::GTest GTest::Main)
target_include_directories(compio_gtest PRIVATE ${CMAKE_SOURCE_DIR}/include)

enable_testing()

add_test(NAME compio_cunit_tests COMMAND compio_cunit)
add_test(NAME compio_gtest_tests COMMAND compio_gtest)