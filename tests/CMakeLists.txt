find_package(GTest REQUIRED)

add_executable(compio_gtest
    test_runner.cpp
    src/test_allocator.cpp
    src/test_allocator_comprehensive.cpp
    src/test_allocator_advanced.cpp
    src/test_allocator_edge_cases.cpp
    src/test_allocator_serialization.cpp
    src/test_allocator_strategies.cpp
    src/test_compression_type.cpp
    src/test_compressor.cpp
    src/test_data_integrity.cpp
    src/test_defragmentation.cpp
    src/test_defragmentation_advanced.cpp
    src/test_library.cpp
    src/test_file_removal.cpp
    src/test_files_table_lifecycle.cpp
    src/test_flat_map.cpp
    src/test_btree.cpp
    src/test_btree_advanced.cpp
    src/test_btree_edge_cases.cpp
    src/test_btree_range_add.cpp
    src/test_btree_get_block.cpp
    src/test_insert_erase.cpp
    src/test_segregated_allocator.cpp
    src/test_max_files.cpp
    src/test_concurrency.cpp
    src/test_concurrency_advanced.cpp
    src/test_header_validation.cpp
    src/test_max_files_scalability.cpp
    src/test_max_files_auto.cpp
    src/test_crash_protection.cpp
    src/test_wal_recovery.cpp
    src/test_wal_format.cpp
    src/test_auto_checkpoint.cpp
)

target_link_libraries(compio_gtest PRIVATE compio GTest::GTest GTest::Main)
target_include_directories(compio_gtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()

add_test(NAME compio_gtest_tests COMMAND compio_gtest)
