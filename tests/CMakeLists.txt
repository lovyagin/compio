cmake_minimum_required(VERSION 3.10)

file(GLOB_RECURSE TEST_SOURCES_C ${CMAKE_SOURCE_DIR}/tests/unit/*.c)
file(GLOB_RECURSE TEST_SOURCES_CPP ${CMAKE_SOURCE_DIR}/tests/unit/*.cpp)

add_executable(compio_tests
        ${TEST_SOURCES_C}
        ${TEST_SOURCES_CPP}
        test_runner.cpp

        unit/zlib_adapter/test_zlib_adapter.cpp
)

target_compile_definitions(compio_tests PRIVATE CUNIT_DISABLE_TEST_SNPRINTF)

target_link_libraries(compio_tests PRIVATE compio_lib unofficial::cunit::cunit)
target_link_libraries(compio_tests PRIVATE ZLIB::ZLIB)

target_include_directories(compio_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(compio_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit/include)

enable_testing()

add_test(NAME compio_unit_tests COMMAND compio_tests)
