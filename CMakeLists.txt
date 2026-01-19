
cmake_minimum_required(VERSION 3.16)
project(TinyWebServer VERSION 3.0 LANGUAGES CXX)

# --- 1. 编译选项与开关 ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 增加开关：默认开启 ASan（安全性优先），性能测试时手动通过 -DENABLE_ASAN=OFF 关闭
option(ENABLE_ASAN "Enable AddressSanitizer for debugging" ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 通用优化标志：-march=native 允许编译器使用当前 CPU 的所有特指令集 (如 AVX2)
set(COMMON_FLAGS "-Wall -Wextra -g -fno-omit-frame-pointer -march=native")

# 处理 Sanitizer 逻辑
if(ENABLE_ASAN)
    set(SAN_FLAGS "-fsanitize=address")
    message(STATUS "Build with ASan: ENABLED (Safety first, lower performance)")
else()
    set(SAN_FLAGS "")
    message(STATUS "Build with ASan: DISABLED (Full performance mode)")
endif()

# Debug 模式
set(CMAKE_CXX_FLAGS_DEBUG "${COMMON_FLAGS} -O0")

# Release 模式：应用极致优化
set(CMAKE_CXX_FLAGS_RELEASE "${COMMON_FLAGS} -O3 ${SAN_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${SAN_FLAGS}")

# --- 2. 依赖与核心库 ---
find_package(Threads REQUIRED)

# 使用 Interface 库统一管理包含路径
add_library(project_configs INTERFACE)
target_include_directories(project_configs INTERFACE 
    ${PROJECT_SOURCE_DIR}/include 
    ${PROJECT_SOURCE_DIR}/include/reactor
    ${PROJECT_SOURCE_DIR}/test
)

# 核心静态库源文件
file(GLOB CORE_SOURCES "src/*.cpp")
list(REMOVE_ITEM CORE_SOURCES "${PROJECT_SOURCE_DIR}/src/main.cpp")

add_library(webserver_core STATIC ${CORE_SOURCES})
target_link_libraries(webserver_core PUBLIC Threads::Threads project_configs)

# --- 3. 构建主程序 ---
add_executable(server src/main.cpp)
target_link_libraries(server PRIVATE webserver_core)

# --- 4. 自动化测试分类管理 ---

# A. 功能与集成测试
set(INTEGRATION_TESTS
    test_timer
    test_lifecycle
    test_single_connection
    test_multi_connection
    test_client_close
    test_backpressure
    test_stress
    test_main
)

# B. 性能基准测试
set(BENCHMARK_TESTS
    test_log_bench
)

# 批量生成集成测试目标
foreach(t ${INTEGRATION_TESTS})
    add_executable(${t} test/${t}.cpp)
    target_link_libraries(${t} PRIVATE webserver_core)
endforeach()

# 批量生成基准测试目标
foreach(b ${BENCHMARK_TESTS})
    add_executable(${b} test/${b}.cpp)
    target_link_libraries(${b} PRIVATE webserver_core)
endforeach()