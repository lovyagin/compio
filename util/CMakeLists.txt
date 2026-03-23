add_executable(compio_unpack unpack.cpp)
target_link_libraries(compio_unpack PRIVATE compio)

add_executable(compio_repair repair.cpp)
target_link_libraries(compio_repair PRIVATE compio)
