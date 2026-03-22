add_executable(unpack unpack.cpp)
target_link_libraries(unpack PRIVATE compio)

add_executable(repair repair.cpp)
target_link_libraries(repair PRIVATE compio)
