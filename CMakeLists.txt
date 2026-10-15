# =============================================================================
# TinyWebServer — 多标准 / 多编译器 / 多平台构建配置
# -----------------------------------------------------------------------------
# 设计要点（与门禁规则一一对应）：
#   1. 标准可切换：TWS_CXX_STANDARD=17|20|23（默认 23），严格模式（无 gnu 扩展），
#      由 STD-001 白名单约束实际可用特性（见 doc/cxx_standard_matrix.md）。
#   2. 平台可切换：WIN32 -> 使用 include/platform/win32 兼容层；否则 POSIX 原生。
#   3. 警告策略与编译器绑定，不使用任何"只在某个编译器下存在"的裸 flag。
#   4. 不依赖 bash/grep 等外部 shell 工具（Windows 与 WSL2 行为一致）。
#   5. 测试按能力打标签：portable（跨平台可运行）与 socket（Windows 上运行验收在 WSL2）。
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(TinyWebServer VERSION 8.0 LANGUAGES CXX)

include(CTest)

if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

# --- 0. 用户可配置选项 -------------------------------------------------------
set(TWS_CXX_STANDARD "23" CACHE STRING "C++ 标准版本：17 / 20 / 23")
set_property(CACHE TWS_CXX_STANDARD PROPERTY STRINGS 17 20 23)

option(TWS_WERROR        "把警告当错误（CI 严格模式）"            OFF)
option(TWS_NATIVE_ARCH   "启用 -march=native（本机专用，CI 关闭）" OFF)
option(TWS_ENABLE_CLANG_TIDY "构建时运行 clang-tidy（需已安装）"   OFF)

option(ENABLE_ASAN "Debug 下启用 AddressSanitizer" ON)
option(ENABLE_UBSAN "启用 UndefinedBehaviorSanitizer" OFF)
option(BUILD_BENCHMARKS "构建性能基准测试框架" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

set(CMAKE_CXX_STANDARD ${TWS_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # 严格 -std=c++NN，保证跨工具链一致
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # 供 clangd/VS Code Problems 面板使用
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 输出目录统一到构建根，便于脚本与门禁定位产物
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR})
endforeach()

# --- 1. 平台与编译器探测 -----------------------------------------------------
if(WIN32)
    set(TWS_PLATFORM "win32")
else()
    set(TWS_PLATFORM "posix")
endif()

if(MSVC)
    set(TWS_COMPILER_ID "msvc")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(TWS_COMPILER_ID "clang")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(TWS_COMPILER_ID "gcc")
else()
    set(TWS_COMPILER_ID "unknown")
endif()

message(STATUS "=========================================")
message(STATUS "TinyWebServer 构建配置")
message(STATUS "  编译器     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ 标准   : c++${TWS_CXX_STANDARD} (严格模式, 扩展=${CMAKE_CXX_EXTENSIONS})")
message(STATUS "  平台       : ${TWS_PLATFORM} (${CMAKE_SYSTEM_NAME})")
message(STATUS "  构建类型   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  ASan/UBSan : ${ENABLE_ASAN}/${ENABLE_UBSAN}")
message(STATUS "  Werror     : ${TWS_WERROR}")
message(STATUS "  测试/基准  : BUILD_TESTING=${BUILD_TESTING} BUILD_BENCHMARKS=${BUILD_BENCHMARKS}")
message(STATUS "=========================================")

# --- 2. 编译标志（按编译器分支，避免"某个编译器不认识的 flag"） ---------------
set(TWS_WARN_FLAGS "")
if(TWS_COMPILER_ID STREQUAL "gcc" OR TWS_COMPILER_ID STREQUAL "clang")
    list(APPEND TWS_WARN_FLAGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(TWS_WERROR)
        list(APPEND TWS_WARN_FLAGS -Werror)
    endif()
elseif(TWS_COMPILER_ID STREQUAL "msvc")
    list(APPEND TWS_WARN_FLAGS /W4)
    if(TWS_WERROR)
        list(APPEND TWS_WARN_FLAGS /WX)
    endif()
endif()

set(TWS_COMMON_FLAGS "")
if(TWS_COMPILER_ID STREQUAL "gcc" OR TWS_COMPILER_ID STREQUAL "clang")
    list(APPEND TWS_COMMON_FLAGS -g -fno-omit-frame-pointer)
    if(TWS_NATIVE_ARCH)
        list(APPEND TWS_COMMON_FLAGS -march=native)
    endif()
endif()

# 严格标准模式下，POSIX 扩展（accept4/SOCK_NONBLOCK/EFD_* 等）需要显式打开
if(TWS_PLATFORM STREQUAL "posix")
    add_compile_definitions(_GNU_SOURCE)
endif()

set(TWS_SANITIZE_FLAGS "")
if(TWS_COMPILER_ID STREQUAL "gcc" OR TWS_COMPILER_ID STREQUAL "clang")
    if(ENABLE_ASAN AND NOT WIN32)
        list(APPEND TWS_SANITIZE_FLAGS -fsanitize=address)
    elseif(ENABLE_ASAN AND WIN32)
        message(STATUS "  提示       : Windows(MinGW) 下默认不启用 ASan（运行库支持不稳定），如需请显式 -DENABLE_ASAN=ON 并自行确认 libasan 可用")
    endif()
    if(ENABLE_UBSAN)
        list(APPEND TWS_SANITIZE_FLAGS -fsanitize=undefined)
    endif()
elseif(TWS_COMPILER_ID STREQUAL "msvc")
    if(ENABLE_ASAN)
        list(APPEND TWS_SANITIZE_FLAGS /fsanitize=address)
    endif()
endif()

# --- 3. 依赖 -----------------------------------------------------------------
find_package(Threads REQUIRED)

find_package(GTest QUIET)
if(GTest_FOUND)
    message(STATUS "GTest: ${GTest_VERSION}")
else()
    message(STATUS "GTest: 未找到 — 依赖 gtest 的测试目标将被跳过（Linux 上 apt install libgtest-dev）")
endif()

find_package(nlohmann_json 3.11.2 QUIET)
if(nlohmann_json_FOUND)
    message(STATUS "nlohmann/json: find_package")
elseif(EXISTS ${PROJECT_SOURCE_DIR}/third_party/json.hpp)
    add_library(nlohmann_json_local INTERFACE)
    target_include_directories(nlohmann_json_local INTERFACE ${PROJECT_SOURCE_DIR}/third_party)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json_local)
    message(STATUS "nlohmann/json: third_party/json.hpp（本地头文件）")
else()
    message(FATAL_ERROR "缺少 nlohmann/json：请放置 third_party/json.hpp 或安装 nlohmann-json3-dev")
endif()

# --- 4. 公共接口库 -----------------------------------------------------------
add_library(project_configs INTERFACE)
target_include_directories(project_configs INTERFACE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/reactor
    ${PROJECT_SOURCE_DIR}/include/common
    ${PROJECT_SOURCE_DIR}/include/error
    ${PROJECT_SOURCE_DIR}/include/timer
    ${PROJECT_SOURCE_DIR}/include/config
    ${PROJECT_SOURCE_DIR}/include/logging
    ${PROJECT_SOURCE_DIR}/include/http
    ${PROJECT_SOURCE_DIR}/include/http2
    ${PROJECT_SOURCE_DIR}/include/plugin
    ${PROJECT_SOURCE_DIR}/include/platform
    ${PROJECT_SOURCE_DIR}/third_party
    ${PROJECT_SOURCE_DIR}/test
)

if(TWS_PLATFORM STREQUAL "win32")
    # Windows 只构建可移植核心：不开 POSIX 兼容头层（架构上不伪装成 Linux）
    target_compile_definitions(project_configs INTERFACE
        WIN32_LEAN_AND_MEAN=1
        NOMINMAX=1)
endif()

target_compile_options(project_configs INTERFACE
    ${TWS_WARN_FLAGS} ${TWS_COMMON_FLAGS} ${TWS_SANITIZE_FLAGS})
target_link_options(project_configs INTERFACE ${TWS_SANITIZE_FLAGS})

if(TWS_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE})
    else()
        message(WARNING "TWS_ENABLE_CLANG_TIDY=ON 但未找到 clang-tidy，忽略")
    endif()
endif()

# --- 5. 核心源文件（按可移植性二分） -----------------------------------------
# 为什么要二分（用户 2026-10-15 决策 B）：
#   本项目的技术身份是 Linux 专有机制 —— epoll、边缘触发 ET、SO_REUSEPORT、
#   eventfd、mmap 零拷贝。在 Windows 上强行移植只能退化成 WSAPoll(O(n)) 的
#   Proactor/轮询替代品，既违反"禁止从 Reactor 切到 Proactor"的架构约束，
#   又会让人误以为 Windows 上具备 epoll 级并发能力。
#   因此：Windows 只构建/测试【平台无关核心】，网络层显式标记为 Linux only。
set(PORTABLE_SOURCES
    src/error.cpp
    src/error/error.cpp
    src/timer/timer_wheel.cpp
    src/timer_manager.cpp
    src/thread_pool.cpp
    src/Logger.cpp
    src/async_Logger.cpp
    src/config/server_config.cpp
    src/server_metrics.cpp
    src/logging/structured_logger.cpp
    src/memory_pool.cpp
    src/http_request.cpp
    src/http_parser.cpp
    src/http_response.cpp
    src/request_validator.cpp
    src/http/keep_alive_manager.cpp
    src/http/conditional_request_handler.cpp
)

# 依赖 POSIX/epoll 的源文件：仅在非 Windows 平台构建
set(NETWORK_SOURCES
    src/server.cpp
    src/connection.cpp
    src/static_resource_manager.cpp
    reactor/event_loop.cpp
    reactor/event_loop_thread.cpp
    reactor/event_loop_thread_pool.cpp
    reactor/socket_utils.cpp
    reactor/multi_listen_socket.cpp
    reactor/batch_io_handler.cpp
    src/http2/h2_frame_parser.cpp
    src/http2/h2_connection.cpp
    src/http2/h2_stream.cpp
    src/http2/hpack_decoder.cpp
    src/plugin/plugin_manager.cpp
    src/plugin/example_plugin.cpp
)

set(MISSING_CORE "")
foreach(src ${PORTABLE_SOURCES} ${NETWORK_SOURCES})
    if(NOT EXISTS ${PROJECT_SOURCE_DIR}/${src})
        list(APPEND MISSING_CORE ${src})
    endif()
endforeach()
if(MISSING_CORE)
    message(FATAL_ERROR "核心源文件缺失: ${MISSING_CORE}")
endif()
list(LENGTH PORTABLE_SOURCES PORTABLE_COUNT)
list(LENGTH NETWORK_SOURCES NETWORK_COUNT)

if(TWS_PLATFORM STREQUAL "win32")
    message(STATUS "源文件: 可移植 ${PORTABLE_COUNT} 个（Windows 不构建网络层 ${NETWORK_COUNT} 个：依赖 epoll/ET/SO_REUSEPORT）")
    add_library(webserver_core STATIC ${PORTABLE_SOURCES})
else()
    message(STATUS "源文件: 可移植 ${PORTABLE_COUNT} 个 + 网络层 ${NETWORK_COUNT} 个（完整性已校验）")
    add_library(webserver_core STATIC ${PORTABLE_SOURCES} ${NETWORK_SOURCES})
endif()
target_link_libraries(webserver_core PUBLIC Threads::Threads project_configs nlohmann_json::nlohmann_json)

# --- 6. 运行时目录（不依赖 bash） --------------------------------------------
file(MAKE_DIRECTORY ${PROJECT_SOURCE_DIR}/public
                    ${PROJECT_SOURCE_DIR}/local/logs
                    ${PROJECT_SOURCE_DIR}/local/logs/benchmark)
if(NOT EXISTS ${PROJECT_SOURCE_DIR}/public/index.html)
    file(WRITE ${PROJECT_SOURCE_DIR}/public/index.html "<h1>TinyWebServer V8</h1>\n")
endif()
add_custom_target(create_runtime_dirs ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_SOURCE_DIR}/public
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_SOURCE_DIR}/local/logs
    COMMENT "运行时目录就绪"
    VERBATIM)

# --- 7. 主程序（网络层，仅 POSIX） -------------------------------------------
if(TWS_PLATFORM STREQUAL "win32")
    message(STATUS "跳过 server 目标：Windows 不构建网络层（详见 SPEC.md 第 2 节平台策略）")
elseif(EXISTS ${PROJECT_SOURCE_DIR}/src/main.cpp)
    add_executable(server src/main.cpp)
    target_link_libraries(server PRIVATE webserver_core project_configs)
else()
    message(FATAL_ERROR "缺少主程序入口 src/main.cpp")
endif()

# 可移植测试：Windows 与 Linux 都必须通过（Windows 的达标口径就是这一组）
set(TESTS_PORTABLE_TARGETS
    test_timer
    test_log
    test_bounded_logger
    test_structured_log
    test_request_validator
    test_keep_alive
    test_conditional_request
    test_memory_pool
    test_cpp_compat)

# 网络/集成测试：依赖 epoll + socket，仅 POSIX 构建；Windows 上的运行验收在 WSL2
set(TESTS_LINUX_TARGETS
    test_lifecycle
    test_single_connection
    test_multi_connection
    test_client_close
    test_backpressure
    test_stress
    test_main
    test_multithread_reactor
    test_basic
    test_multi_listen_socket
    test_batch_io_handler
    test_so_reuseport_integration)

set(TESTS_NEEDING_GTEST
    test_multi_listen_socket
    test_batch_io_handler
    test_so_reuseport_integration)

set(TESTS_SMOKE
    test_timer
    test_basic
    test_multithread_reactor
    test_log
    test_bounded_logger)

set(ALL_TEST_TARGETS "")
if(BUILD_TESTING)
    set(TEST_TARGETS ${TESTS_PORTABLE_TARGETS})
    set(TESTS_PORTABLE ${TESTS_PORTABLE_TARGETS})
    if(NOT TWS_PLATFORM STREQUAL "win32")
        list(APPEND TEST_TARGETS ${TESTS_LINUX_TARGETS})
    endif()

    foreach(test_name ${TEST_TARGETS})
        set(TEST_FILE ${PROJECT_SOURCE_DIR}/test/${test_name}.cpp)
        if(NOT EXISTS ${TEST_FILE})
            message(WARNING "跳过测试 ${test_name}：${TEST_FILE} 不存在")
            continue()
        endif()

        if(${test_name} IN_LIST TESTS_NEEDING_GTEST AND NOT GTest_FOUND)
            message(STATUS "跳过测试 ${test_name}：需要 GTest")
            continue()
        endif()

        add_executable(${test_name} test/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE webserver_core project_configs nlohmann_json::nlohmann_json)
        if(${test_name} IN_LIST TESTS_NEEDING_GTEST)
            target_link_libraries(${test_name} PRIVATE GTest::gtest GTest::gtest_main)
        endif()

        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 90)

        set(labels "integration")
        if(${test_name} IN_LIST TESTS_SMOKE)
            list(APPEND labels "smoke")
        endif()
        if(${test_name} IN_LIST TESTS_PORTABLE)
            list(APPEND labels "portable")
        else()
            list(APPEND labels "socket")
        endif()
        if(${test_name} IN_LIST TESTS_NEEDING_GTEST)
            list(APPEND labels "gtest")
        endif()
        set_tests_properties(${test_name} PROPERTIES LABELS "${labels}")
        list(APPEND ALL_TEST_TARGETS ${test_name})
    endforeach()
    list(LENGTH ALL_TEST_TARGETS TEST_COUNT)
    message(STATUS "测试目标: ${TEST_COUNT} 个（portable=${TESTS_PORTABLE}）")
endif()

# --- 9. 基准测试框架（网络压测，仅 POSIX） -----------------------------------
if(BUILD_BENCHMARKS AND TWS_PLATFORM STREQUAL "win32")
    message(STATUS "跳过基准测试框架：依赖 socket 压测与 /proc 采样，属 Linux 专有")
    set(BUILD_BENCHMARKS OFF)
endif()

if(BUILD_BENCHMARKS)
    set(BENCHMARK_SOURCES
        benchmark/src/benchmark_base.cpp
        benchmark/src/benchmark_qps.cpp
        benchmark/src/benchmark_latency.cpp
        benchmark/src/benchmark_memory.cpp
        benchmark/src/benchmark_concurrent.cpp
        benchmark/src/benchmark_factory.cpp
        benchmark/src/benchmark_step_stress.cpp
    )
    set(VALID_BENCHMARK_SOURCES "")
    foreach(src ${BENCHMARK_SOURCES})
        if(EXISTS ${PROJECT_SOURCE_DIR}/${src})
            list(APPEND VALID_BENCHMARK_SOURCES ${PROJECT_SOURCE_DIR}/${src})
        else()
            message(WARNING "基准测试源文件缺失: ${src}")
        endif()
    endforeach()

    if(EXISTS ${PROJECT_SOURCE_DIR}/benchmark/src/benchmark_runner.cpp)
        add_executable(benchmark_runner benchmark/src/benchmark_runner.cpp ${VALID_BENCHMARK_SOURCES})
        target_link_libraries(benchmark_runner PRIVATE webserver_core project_configs nlohmann_json::nlohmann_json)
    else()
        message(WARNING "缺少 benchmark/src/benchmark_runner.cpp")
    endif()

    set(BENCHMARK_CONFIGS
        qps_config.json latency_config.json memory_config.json
        concurrent_config.json step_stress_config.json)
    set(COPY_CONFIG_CMDS "")
    foreach(cfg ${BENCHMARK_CONFIGS})
        if(EXISTS ${PROJECT_SOURCE_DIR}/configs/benchmark/${cfg})
            list(APPEND COPY_CONFIG_CMDS
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${PROJECT_SOURCE_DIR}/configs/benchmark/${cfg}
                    ${CMAKE_BINARY_DIR}/configs/benchmark/)
        endif()
    endforeach()
    add_custom_target(copy_benchmark_configs ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/configs/benchmark
        ${COPY_CONFIG_CMDS}
        COMMENT "复制基准测试配置"
        VERBATIM)
endif()

# --- 10. 门禁友好的摘要输出 --------------------------------------------------
message(STATUS "=========================================")
message(STATUS "配置完成: c++${TWS_CXX_STANDARD} | ${TWS_PLATFORM} | ${TWS_COMPILER_ID} | ${CMAKE_BUILD_TYPE}")
message(STATUS "  compile_commands.json : ${CMAKE_BINARY_DIR}/compile_commands.json")
message(STATUS "  portable 测试         : ctest -L portable")
message(STATUS "  smoke 测试            : ctest -L smoke")
message(STATUS "=========================================")
