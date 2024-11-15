cmake_minimum_required(VERSION 3.10)

file(GLOB_RECURSE TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/*.c)

add_executable(compio_tests ${TEST_SOURCES} ${CMAKE_SOURCE_DIR}/src/block.c)

target_link_libraries(compio_tests PRIVATE ZLIB::ZLIB unofficial::cunit::cunit)

target_include_directories(compio_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

enable_testing()
add_test(NAME compio_unit_tests COMMAND compio_tests)