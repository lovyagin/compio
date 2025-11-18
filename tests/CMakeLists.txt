find_package(GTest REQUIRED)

add_executable(compio_gtest
    test_runner.cpp
    src/test_allocator.cpp
    src/test_allocator_comprehensive.cpp
    src/test_allocator_advanced.cpp
    src/test_allocator_edge_cases.cpp
    src/test_allocator_serialization.cpp
    src/test_compression_type.cpp
    src/test_compressor.cpp
    src/test_library.cpp
    src/test_file_removal.cpp
    src/test_btree.cpp
    src/test_btree_advanced.cpp
    src/test_btree_edge_cases.cpp
    src/test_btree_range_add.cpp
    src/test_insert_erase.cpp
)

target_link_libraries(compio_gtest PRIVATE compio GTest::GTest GTest::Main)
target_include_directories(compio_gtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()

add_test(NAME compio_gtest_tests COMMAND compio_gtest)
