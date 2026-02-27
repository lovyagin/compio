cmake_minimum_required(VERSION 3.15)

project(compio LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Preconfiguration

# build Release by default
if (NOT MSVC AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

IF (MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# use vcpkg toolchain by default
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "CMake toolchain file")
    endif()
endif()


option(COMPIO_DEBUG_PRINT "compio: enable DEBUG prints" OFF)
option(COMPIO_WARNING_PRINT "compio: enable WARNING prints" ON)
option(COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER "compio: enable filesystem operations counting for benchmarking" OFF)
option(COMPIO_BENCHMARK_BLOCKS_COUNTER "compio: enable blocks counting for benchmarking" OFF)
option(COMPIO_BENCHMARK_COMPRESSION_BYTES "compio: enable compression bytes counting" OFF)
option(COMPIO_DISABLE_INSERT_ERASE "compio: disable insert/erase operations" OFF)

if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if (NOT DEFINED COMPIO_DEBUG_PRINT)
        set(COMPIO_DEBUG_PRINT ON)
    endif()
    if (NOT DEFINED COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER)
        set(COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER ON)
    endif()
endif()

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if (COMPIO_WARNING_PRINT)
    message("Building with COMPIO_WARNING_PRINT")
    add_compile_options(-DCOMPIO_WARNING_PRINT)
endif()

if (COMPIO_DEBUG_PRINT)
    message("Building with COMPIO_DEBUG_PRINT")
    add_compile_options(-DCOMPIO_DEBUG_PRINT)
endif()

if (COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER)
    message("Building with COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER")
    add_compile_options(-DCOMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER)
endif()

if (COMPIO_BENCHMARK_BLOCKS_COUNTER)
    message("Building with COMPIO_BENCHMARK_BLOCKS_COUNTER")
    add_compile_options(-DCOMPIO_BENCHMARK_BLOCKS_COUNTER)
endif()

if (COMPIO_BENCHMARK_COMPRESSION_BYTES)
    message("Building with COMPIO_BENCHMARK_COMPRESSION_BYTES")
    add_compile_options(-DCOMPIO_BENCHMARK_COMPRESSION_BYTES)
endif()

if (COMPIO_DISABLE_INSERT_ERASE)
    message("Building with COMPIO_DISABLE_INSERT_ERASE")
    add_compile_options(-DCOMPIO_DISABLE_INSERT_ERASE)
endif()


option(COMPIO_SANITIZE_ADDRESS   "compio: enable address sanitizer"   OFF)
option(COMPIO_SANITIZE_UNDEFINED "compio: enable undefined sanitizer" OFF)

if (NOT MSVC)
    if (COMPIO_SANITIZE_ADDRESS)
        message(STATUS "Using -fsanitize=address")

        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        link_libraries     (-fsanitize=address)
    endif()

    if (COMPIO_SANITIZE_UNDEFINED)
        message(STATUS "Using -fsanitize=undefined")

        add_compile_options(-fsanitize=undefined)
        link_libraries     (-fsanitize=undefined)
    endif()
endif()

# --- Library target

add_library(compio SHARED
    src/allocator.cpp
    src/btree.cpp
    src/compio.cpp
    src/compressor.cpp
    src/file.cpp
    src/infile_object.cpp
    src/sha256.cpp
    src/storage_block_reader.cpp
    src/utils.cpp
)

set_target_properties(compio PROPERTIES PUBLIC_HEADER compio.h)

target_include_directories(compio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(compio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(compio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

include(GNUInstallDirs)
install(TARGETS compio
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

target_compile_definitions(compio PRIVATE
    $<$<BOOL:${COMPIO_DEBUG_PRINT}>:COMPIO_DEBUG_PRINT=1>
    $<$<BOOL:${COMPIO_WARNING_PRINT}>:COMPIO_WARNING_PRINT=1>
    $<$<BOOL:${COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER}>:COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER=1>
    $<$<BOOL:${COMPIO_BENCHMARK_BLOCKS_COUNTER}>:COMPIO_BENCHMARK_BLOCKS_COUNTER=1>
    $<$<BOOL:${COMPIO_BENCHMARK_COMPRESSION_BYTES}>:COMPIO_BENCHMARK_COMPRESSION_BYTES=1>
    $<$<BOOL:${COMPIO_DISABLE_INSERT_ERASE}>:COMPIO_DISABLE_INSERT_ERASE=1>
)

# --- Dependencies

find_package(ZLIB REQUIRED)
target_link_libraries(compio PRIVATE ZLIB::ZLIB)

find_package(lz4 CONFIG REQUIRED)
target_link_libraries(compio PRIVATE lz4::lz4)

find_package(zstd CONFIG REQUIRED)
target_link_libraries(compio PRIVATE $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)

find_package(unofficial-brotli CONFIG QUIET)
if(unofficial-brotli_FOUND)
    target_link_libraries(compio PRIVATE 
        unofficial::brotli::brotlienc 
        unofficial::brotli::brotlidec 
        unofficial::brotli::brotlicommon
    )
else()
    find_package(brotli CONFIG REQUIRED)
    target_link_libraries(compio PRIVATE 
        brotli::brotlienc 
        brotli::brotlidec 
        brotli::brotlicommon
    )
endif()

# --- Subdirectories

enable_testing()

add_subdirectory(tests)
add_subdirectory(benchmarks)
add_subdirectory(examples)
add_subdirectory(util)