add_executable(unpack unpack.cpp)

target_link_libraries(unpack PRIVATE compio)

target_include_directories(unpack PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
