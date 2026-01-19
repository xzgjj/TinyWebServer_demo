
cmake_minimum_required(VERSION 3.16)
project(TinyWebServer VERSION 3.0 LANGUAGES CXX)

# --- 1. 基础标准设置 ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 如果没指定模式，默认使用 Release 模式（为了跑 -O3）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- 2. 核心编译标志设置 ---
# 无论 Debug 还是 Release，都建议保留 -g 以便在崩溃时看到堆栈
set(COMMON_FLAGS "-Wall -Wextra -Wno-unused-variable -g -fno-omit-frame-pointer")

# Debug 模式：-O0，方便 GDB 调试逻辑
set(CMAKE_CXX_FLAGS_DEBUG "${COMMON_FLAGS} -O0")

# Release 模式：-O3 极致优化 + ASan 内存审计
# 注意：开启 ASan 后，运行时严禁挂载 Valgrind，两者会冲突导致 SIGSEGV
set(CMAKE_CXX_FLAGS_RELEASE "${COMMON_FLAGS} -O3 -fsanitize=address")

# 确保链接阶段也加入 ASan 库
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-fsanitize=address")

# --- 3. 查找依赖 ---
find_package(Threads REQUIRED)

# --- 4. 包含路径 ---
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/include/reactor)
include_directories(${PROJECT_SOURCE_DIR}/test)

# --- 5. 构建核心静态库 (webserver_core) ---
# 确保所有 V3 的源文件都在这里
add_library(webserver_core STATIC
    src/epoll_reactor.cpp
    src/connection.cpp
    src/socket_utils.cpp
    src/error.cpp
    src/http_request.cpp
    src/server.cpp
    src/thread_pool.cpp
)
target_link_libraries(webserver_core PRIVATE Threads::Threads)

# --- 6. 构建主程序 ---
add_executable(server src/main.cpp)
target_link_libraries(server PRIVATE webserver_core)

# --- 7. 构建测试集 ---
set(TEST_PROGS
    test_lifecycle
    test_single_connection
    test_multi_connection
    test_client_close
    test_backpressure
    test_stress
    test_main
)

foreach(t ${TEST_PROGS})
    add_executable(${t} test/${t}.cpp)
    target_link_libraries(${t} PRIVATE webserver_core)
endforeach()