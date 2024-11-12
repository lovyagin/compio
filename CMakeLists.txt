cmake_minimum_required(VERSION 3.10)

project(compio LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

# Создание исполняемого файла
add_executable(compio main.c
        include/block.h)

file(GLOB ...)

# Подключение всех заголовочных и исходных файлов
target_sources(compio PRIVATE
        src/block.c
        src/compression.c
        src/storage.c
        archivers/squash_adapter.c
        archivers/zlib_adapter.c
)

# Каталог для заголовочных файлов
target_include_directories(compio PRIVATE include)

# Поиск и подключение ZLIB
find_package(ZLIB REQUIRED)
if(ZLIB_FOUND)
    target_link_libraries(compio PRIVATE ZLIB::ZLIB)
else()
    message(FATAL_ERROR "ZLIB not found. Please install it using vcpkg.")
endif()

# Поиск и подключение CUnit
find_package(unofficial-cunit CONFIG REQUIRED)
if(TARGET unofficial::cunit::cunit)
    target_link_libraries(compio PRIVATE unofficial::cunit::cunit)
else()
    message(FATAL_ERROR "CUnit not found. Please install it using vcpkg.")
endif()