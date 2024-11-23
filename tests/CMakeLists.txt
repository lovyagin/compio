cmake_minimum_required(VERSION 3.10)

file(GLOB_RECURSE TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/*.c)

add_executable(compio_tests
        test_runner.c
        ${TEST_SOURCES}
)

target_compile_definitions(compio_tests PRIVATE CUNIT_DISABLE_TEST_SNPRINTF)

target_link_libraries(compio_tests PRIVATE compio_lib unofficial::cunit::cunit)

target_include_directories(compio_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

enable_testing()
add_test(NAME compio_unit_tests COMMAND compio_tests)
