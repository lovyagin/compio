cmake_minimum_required(VERSION 3.10)

project(compio LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS True)
endif()

# add_compile_definitions(WARNING_PRINT_ENABLED)
# add_compile_definitions(DEBUG_PRINT_ENABLED)
add_compile_definitions(BM_FILE_OPERATIONS_COUNTER)
set(CMAKE_BUILD_TYPE RelWithDebInfo)

set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

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

find_package(ZLIB REQUIRED)
target_link_libraries(compio PRIVATE ZLIB::ZLIB)

find_package(unofficial-cunit CONFIG REQUIRED)
target_link_libraries(compio PRIVATE unofficial::cunit::cunit)
    
# find_package(CUnit REQUIRED)
# target_link_libraries(compio PRIVATE CUnit)

if (MSVC)
    target_compile_definitions(compio PRIVATE _SILENCE_CXX17_C_HEADER_DEPRECATION_WARNING)
endif()

add_subdirectory(tests)
add_subdirectory(benchmarks)
add_subdirectory(examples)
add_subdirectory(util)