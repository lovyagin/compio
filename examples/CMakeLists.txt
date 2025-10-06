add_executable(example_usage example_usage.c)

target_link_libraries(example_usage PRIVATE compio)

target_include_directories(example_usage PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(compressor_example compressor_example.c)

target_link_libraries(compressor_example PRIVATE compio)

target_include_directories(compressor_example PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(compression_persistence_test compression_persistence_test.c)

target_link_libraries(compression_persistence_test PRIVATE compio)

target_include_directories(compression_persistence_test PRIVATE ${CMAKE_SOURCE_DIR})
