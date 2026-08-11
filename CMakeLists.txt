cmake_minimum_required(VERSION 3.16)
project(lockfree_datastructures CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

add_executable(queue_benchmark benchmark/queue_benchmark.cpp)
target_link_libraries(queue_benchmark pthread)

add_executable(hashmap_benchmark benchmark/hashmap_benchmark.cpp)
target_link_libraries(hashmap_benchmark pthread)

enable_testing()
add_executable(test_mpmc_queue tests/test_mpmc_queue.cpp)
target_link_libraries(test_mpmc_queue pthread)
add_test(NAME mpmc_queue_test COMMAND test_mpmc_queue)

add_executable(test_hash_map tests/test_hash_map.cpp)
target_link_libraries(test_hash_map pthread)
add_test(NAME hash_map_test COMMAND test_hash_map)
