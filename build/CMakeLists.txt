cmake_minimum_required(VERSION 3.10)
project(cnet_build_tool LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(agent_memory STATIC
    src/agent_memory.c
)
target_include_directories(agent_memory
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_executable(build_tool build.c)
target_link_libraries(build_tool PRIVATE agent_memory)

enable_testing()

add_executable(agent_memory_tests tests/test_agent_memory.c src/agent_memory.c)
target_include_directories(agent_memory_tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
add_test(NAME agent_memory_tests COMMAND agent_memory_tests)
