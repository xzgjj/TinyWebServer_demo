
cmake_minimum_required(VERSION 3.16)
project(TinyWebServer VERSION 3.0 LANGUAGES CXX)

# --- 1. 编译选项与开关 ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ✅ 修复1：优化默认行为 - Debug模式默认开启ASan，Release模式关闭
option(ENABLE_ASAN "Enable AddressSanitizer for debugging" ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "=========================================")

# 通用优化标志
set(COMMON_FLAGS "-Wall -Wextra -g -fno-omit-frame-pointer -march=native")

# ✅ 修复2：正确处理Debug和Release模式的ASan配置
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND ENABLE_ASAN)
    set(DEBUG_FLAGS "${COMMON_FLAGS} -O0 -fsanitize=address -fsanitize=undefined")
    set(LINKER_FLAGS_DEBUG "-fsanitize=address -fsanitize=undefined")
    message(STATUS "Debug模式: ASan ENABLED")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DEBUG_FLAGS "${COMMON_FLAGS} -O0")
    set(LINKER_FLAGS_DEBUG "")
    message(STATUS "Debug模式: ASan DISABLED")
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(RELEASE_FLAGS "${COMMON_FLAGS} -O3 -DNDEBUG")
    set(LINKER_FLAGS_RELEASE "")
    message(STATUS "Release模式: 性能优化 ENABLED")
endif()

# 设置编译标志
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS "${DEBUG_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${LINKER_FLAGS_DEBUG}")
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${RELEASE_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${LINKER_FLAGS_RELEASE}")
endif()

# ✅ 修复3：设置所有可执行文件输出到build根目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR})

# --- 2. 依赖与核心库 ---
find_package(Threads REQUIRED)

# ✅ 修复4：完整包含路径
add_library(project_configs INTERFACE)
target_include_directories(project_configs INTERFACE 
    ${PROJECT_SOURCE_DIR}/include 
    ${PROJECT_SOURCE_DIR}/include/reactor
    ${PROJECT_SOURCE_DIR}/include/common
    ${PROJECT_SOURCE_DIR}/test
)

# ✅ 修复5：源文件完整性检查
message(STATUS "=========================================")
message(STATUS "源文件结构自检")
message(STATUS "=========================================")

# 核心源文件列表（手动指定以确保完整性）
set(CORE_SOURCES_LIST
    src/server.cpp
    src/connection.cpp
    src/http_request.cpp
    src/http_parser.cpp
    src/thread_pool.cpp
    src/Logger.cpp
    src/async_Logger.cpp
    reactor/event_loop.cpp
    reactor/event_loop_thread.cpp
    reactor/event_loop_thread_pool.cpp
    reactor/socket_utils.cpp
)

# 检查每个源文件是否存在
set(VALID_CORE_SOURCES "")
foreach(src ${CORE_SOURCES_LIST})
    if(EXISTS ${PROJECT_SOURCE_DIR}/${src})
        list(APPEND VALID_CORE_SOURCES ${PROJECT_SOURCE_DIR}/${src})
        message(STATUS "✓ 确认: ${src}")
    else()
        message(WARNING "⚠️ 缺失: ${src}")
    endif()
endforeach()

# 如果没有找到任何有效源文件，则使用file(GLOB)作为备选
if("${VALID_CORE_SOURCES}" STREQUAL "")
    message(STATUS "未找到手动指定的源文件，尝试自动搜索...")
    file(GLOB_RECURSE AUTO_CORE_SOURCES 
        ${PROJECT_SOURCE_DIR}/src/*.cpp
        ${PROJECT_SOURCE_DIR}/reactor/*.cpp
    )
    set(VALID_CORE_SOURCES ${AUTO_CORE_SOURCES})
    foreach(src ${VALID_CORE_SOURCES})
        file(RELATIVE_PATH rel_path ${PROJECT_SOURCE_DIR} ${src})
        message(STATUS "✓ 自动发现: ${rel_path}")
    endforeach()
endif()

# 检查timer_manager.cpp是否存在（如果使用）
set(TIMER_MANAGER_FILE ${PROJECT_SOURCE_DIR}/src/timer_manager.cpp)
if(EXISTS ${TIMER_MANAGER_FILE})
    list(APPEND VALID_CORE_SOURCES ${TIMER_MANAGER_FILE})
    message(STATUS "✓ 添加: src/timer_manager.cpp")
endif()

# 创建核心静态库
if(NOT "${VALID_CORE_SOURCES}" STREQUAL "")
    add_library(webserver_core STATIC ${VALID_CORE_SOURCES})
    target_link_libraries(webserver_core PUBLIC Threads::Threads project_configs)
    message(STATUS "✅ 核心库 webserver_core 配置完成")
else()
    message(FATAL_ERROR "❌ 未找到任何有效的核心源文件")
endif()

# --- 3. 构建主程序 ---
if(EXISTS ${PROJECT_SOURCE_DIR}/src/main.cpp)
    add_executable(server src/main.cpp)
    target_link_libraries(server PRIVATE webserver_core project_configs)
    message(STATUS "✅ 主程序 server 配置完成")
else()
    message(WARNING "⚠️ 未找到主程序入口: src/main.cpp")
endif()

# --- 4. 自动化测试分类管理 ---
message(STATUS "")
message(STATUS "=========================================")
message(STATUS "测试文件加载自检")
message(STATUS "=========================================")

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
    test_multithread_reactor
    test_basic
    test_log
)

# B. 性能基准测试
set(BENCHMARK_TESTS
    test_log_bench
)

# 合并去重
set(ALL_TEST_TARGETS ${INTEGRATION_TESTS})
foreach(test ${BENCHMARK_TESTS})
    list(APPEND ALL_TEST_TARGETS ${test})
endforeach()
list(REMOVE_DUPLICATES ALL_TEST_TARGETS)

# 批量生成测试目标
foreach(test_name ${ALL_TEST_TARGETS})
    set(TEST_FILE ${PROJECT_SOURCE_DIR}/test/${test_name}.cpp)
    if(EXISTS ${TEST_FILE})
        add_executable(${test_name} test/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE webserver_core project_configs)
        message(STATUS "✓ 加载测试用例: ${test_name}")
    else()
        message(WARNING "⚠️ 跳过测试用例: ${test_name} (文件缺失)")
    endif()
endforeach()

# --- 最终提示 ---
message(STATUS "")
message(STATUS "=========================================")
message(STATUS "✅ 项目配置完成 [简洁优化版]")
message(STATUS "编译环境: C++17 | ${CMAKE_BUILD_TYPE}")
message(STATUS "✅ 所有可执行文件将生成在: ${CMAKE_BINARY_DIR}")
message(STATUS "✅ 核心库: webserver_core")
message(STATUS "✅ 测试用例总数: ${ALL_TEST_TARGETS}")
message(STATUS "=========================================")