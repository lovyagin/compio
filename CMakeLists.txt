cmake_minimum_required(VERSION 3.10)

project(compio VERSION 1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(SOURCES
    main.c
    src/allocator.cpp
    src/btree.cpp
    src/compio.cpp
    src/compressor.c
    src/file.cpp
    src/utils.cpp
    src/infile_object.cpp
)

add_executable(compio ${SOURCES})

target_include_directories(compio PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)

# Add tests (assuming you have test files, e.g., test_main.cpp)
# Uncomment and modify the following lines if you have test files.
# set(TEST_SOURCES
#     tests/test_main.cpp  # Add your test source files here
# )

# Uncomment to create a test executable
# add_executable(MyProjectTests ${TEST_SOURCES})
# add_test(NAME MyProjectTests COMMAND MyProjectTests)

install(TARGETS compio DESTINATION bin)
