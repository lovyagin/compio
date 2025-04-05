cmake_minimum_required(VERSION 3.10)

project(compio LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

add_executable(compio
        main.c
)

target_sources(compio PRIVATE
        src/block.c
        src/compressor.c
        src/storage.c
        src/BTree/btree.c

        src/BTree/btree.cpp
        src/allocator.cpp
        src/file.cpp
        src/utils.cpp

        src/archivers/squash_adapter.c
        src/archivers/zlib_adapter.cpp

        src/storage/storage.cpp
        src/storage/storage_block.cpp
)

add_library(compio_lib
        src/block.c
        src/BTree/btree.c

        src/BTree/btree.cpp
        src/allocator.cpp
        src/file.cpp
        src/utils.cpp

        src/archivers/squash_adapter.c
        src/archivers/zlib_adapter.cpp

        src/storage/storage.cpp
        src/storage/storage_block.cpp
)

target_link_libraries(compio PRIVATE compio_lib)

target_include_directories(compio PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}
)

target_include_directories(compio_lib PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}
)

find_package(ZLIB REQUIRED)
if(ZLIB_FOUND)
    target_link_libraries(compio PRIVATE ZLIB::ZLIB)
else()
    message(FATAL_ERROR "ZLIB not found. Please install it using vcpkg.")
endif()

find_package(unofficial-cunit REQUIRED)
if(TARGET unofficial::cunit::cunit)
    target_link_libraries(compio PRIVATE unofficial::cunit::cunit)
else()
    message(FATAL_ERROR "CUnit not found. Please install it using your package manager.")
endif()

if (MSVC)
    target_compile_definitions(compio PRIVATE _SILENCE_CXX17_C_HEADER_DEPRECATION_WARNING)
endif()

if (MSVC)
    target_compile_definitions(compio PRIVATE _SILENCE_CXX17_C_HEADER_DEPRECATION_WARNING)
endif()

add_subdirectory(tests)
