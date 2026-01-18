
cmake_minimum_required(VERSION 3.16)
project(TinyWebServer VERSION 2.0 LANGUAGES CXX) # 升级为 V2.0

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 编译选项：增加调试信息和警告
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wall -Wextra -Wpedantic -g -O0")

# --- 0. 查找多线程库 ---
# 推荐使用这种方式，它能自动处理不同系统的 -pthread 标志
find_package(Threads REQUIRED)

# 包含目录
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/test)

# --- 1. 核心逻辑静态库 ---
# 注意：新增了 src/thread_pool.cpp
add_library(webserver_core STATIC
    src/epoll_reactor.cpp
    src/connection.cpp
    src/socket_utils.cpp
    src/error.cpp
    src/http_parser.cpp
    src/thread_pool.cpp
)

# 链接线程库到核心库
target_link_libraries(webserver_core PRIVATE Threads::Threads)

# --- 2. 主服务器程序 ---
add_executable(server src/main.cpp)
target_link_libraries(server PRIVATE webserver_core)

# --- 3. 自动化测试用例 ---
set(TEST_PROGS
    test_lifecycle
    test_single_connection
    test_multi_connection
    test_client_close
    test_backpressure
    test_stress
    test_main
)

foreach(T_NAME ${TEST_PROGS})
    add_executable(${T_NAME} test/${T_NAME}.cpp)
    target_link_libraries(${T_NAME} PRIVATE webserver_core)
endforeach()

# 启用 CTest
enable_testing()
foreach(T_NAME ${TEST_PROGS})
    add_test(NAME ${T_NAME} COMMAND ${T_NAME})
endforeach()