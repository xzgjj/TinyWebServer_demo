
cmake_minimum_required(VERSION 3.16)
project(TinyWebServer VERSION 3.0 LANGUAGES CXX) # 升级为 V3.0 (HTTP & FSM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 编译选项：增加调试信息和警告
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wall -Wextra -Wpedantic -g -O0")

# --- 0. 查找多线程库 ---
find_package(Threads REQUIRED)

# --- 1. 包含目录 ---
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/include/reactor) # 显式包含 reactor 子目录
include_directories(${PROJECT_SOURCE_DIR}/test)

# --- 2. 核心逻辑静态库 ---
# V3 新增了 server.cpp 和 http_request.cpp
add_library(webserver_core STATIC
    src/epoll_reactor.cpp
    src/connection.cpp
    src/socket_utils.cpp
    src/error.cpp
    src/http_request.cpp   # V3 新增：HTTP 请求解析
    src/server.cpp         # V3 新增：服务器高层封装
    src/thread_pool.cpp
)

# 链接线程库到核心库
target_link_libraries(webserver_core PRIVATE Threads::Threads)

# --- 3. 主服务器程序 ---
add_executable(server src/main.cpp)
target_link_libraries(server PRIVATE webserver_core)

# --- 4. 自动化测试用例 ---
# 注意：V3 的逻辑可能会导致部分 V2 的旧测试用例（如简单 echo 测试）失败，
# 因为服务器现在期望的是 HTTP 报文格式。
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
    if(EXISTS "${PROJECT_SOURCE_DIR}/test/${T_NAME}.cpp")
        add_executable(${T_NAME} test/${T_NAME}.cpp)
        target_link_libraries(${T_NAME} PRIVATE webserver_core)
    endif()
endforeach()

# 启用 CTest
enable_testing()
foreach(T_NAME ${TEST_PROGS})
    if(TARGET ${T_NAME})
        add_test(NAME ${T_NAME} COMMAND ${T_NAME})
    endif()
endforeach()