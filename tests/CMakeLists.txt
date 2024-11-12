cmake_minimum_required(VERSION 3.10)

# Собираем все исходные файлы проекта, которые необходимы для тестов (не исчерпывающий список)
file(GLOB_RECURSE SOURCES
        ${PROJECT_SOURCE_DIR}/src/*.c
        ${PROJECT_SOURCE_DIR}/archivers/*.c
)

# Создание исполняемого файла для тестов
add_executable(compio_tests
        ${TEST_SOURCES}
)

target_link_libraries(compio_tests PRIVATE ZLIB::ZLIB CUnit::CUnit)

target_include_directories(compio_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)

enable_testing()
add_test(NAME compio_unit_tests COMMAND compio_tests)
