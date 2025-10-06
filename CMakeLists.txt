cmake_minimum_required(VERSION 3.10)

project(compio LANGUAGES C CXX)


if (NOT DEFINED CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 11 CACHE STRING "C Standard")
endif()

if (NOT DEFINED CMAKE_C_STANDARD_REQUIRED)
    set(CMAKE_C_STANDARD_REQUIRED ON CACHE STRING "C Standard required")
endif()

if (NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ Standard")
endif()

if (NOT DEFINED CMAKE_CXX_STANDARD_REQUIRED)
    set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE STRING "C++ Standard required")
endif()


# # build Release by default
if (NOT DEFINED CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()


# use vcpkg toolchain by default
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "CMake toolchain file")
    endif()
endif()


add_library(compio SHARED
    src/allocator.cpp
    src/btree.cpp
    src/compio.cpp
    src/compressor.c
    src/file.cpp
    src/infile_object.cpp
    src/storage_block_reader.cpp
    src/utils.cpp
)

set_target_properties(compio PROPERTIES PUBLIC_HEADER compio.h)

target_include_directories(compio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(compio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(compio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

include(GNUInstallDirs)
install(TARGETS compio
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)


# enable DEBUG print on Debug/RelWithDebInfo builds by default
if (NOT DEFINED DEBUG_PRINT_ENABLED)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(DEBUG_PRINT_ENABLED ON CACHE BOOL "Enable DEBUG prints")
    else()
        set(DEBUG_PRINT_ENABLED OFF CACHE BOOL "Enable DEBUG prints")
    endif()
endif()

# enable filesystem operations counting for benchmarking on Debug/RelWithDebInfo builds by default
if (NOT DEFINED BM_FILE_OPERATIONS_COUNTER)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(BM_FILE_OPERATIONS_COUNTER ON CACHE BOOL "Enable filesystem operations counting for benchmarking")
    else()
        set(BM_FILE_OPERATIONS_COUNTER OFF CACHE BOOL "Enable filesystem operations counting for benchmarking")
    endif()
endif()

# enable WARNING print by default on all build types
if (NOT DEFINED WARNING_PRINT_ENABLED)
    set(WARNING_PRINT_ENABLED ON CACHE BOOL "Enable WARNING prints")
endif()

if (DEBUG_PRINT_ENABLED)
    target_compile_definitions(compio PRIVATE DEBUG_PRINT_ENABLED=1)
endif()

if (WARNING_PRINT_ENABLED)
    target_compile_definitions(compio PRIVATE WARNING_PRINT_ENABLED=1)
endif()

if (BM_FILE_OPERATIONS_COUNTER)
    target_compile_definitions(compio PRIVATE BM_FILE_OPERATIONS_COUNTER=1)
endif()


find_package(ZLIB REQUIRED)
target_link_libraries(compio PRIVATE ZLIB::ZLIB)

find_package(lz4 CONFIG REQUIRED)
target_link_libraries(compio PRIVATE lz4::lz4)

find_package(zstd CONFIG REQUIRED)
target_link_libraries(compio PRIVATE $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)

find_package(unofficial-brotli CONFIG REQUIRED)
target_link_libraries(compio PRIVATE unofficial::brotli::brotlienc unofficial::brotli::brotlidec unofficial::brotli::brotlicommon)

add_subdirectory(tests)
add_subdirectory(benchmarks)
add_subdirectory(examples)
add_subdirectory(util)