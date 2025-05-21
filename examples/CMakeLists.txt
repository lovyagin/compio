add_executable(example_usage example_usage.c)

target_link_libraries(example_usage PRIVATE compio)

target_include_directories(example_usage PRIVATE ${CMAKE_SOURCE_DIR})