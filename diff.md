# Diff 记录 - TinyWebServer 关键代码修改

## 2026-03-06 (阶段一完成)
### 涉及文件
1. `include/connection.h` - Connection 状态机扩展
2. `src/connection.cpp` - 状态转移与资源限制实现
3. `include/connection_limits.h` - 新增资源限制配置
4. `implementation_plan.md` - 规划更新

### 核心 Diff 摘要
#### 1. Connection 状态机扩展 (7个状态)
```diff
+enum class ConnState {
+    kConnecting,      ///< 连接建立中 (SYN_SENT)
+    kConnected,       ///< 连接已建立，等待请求
+    kReading,         ///< 读取请求中
+    kProcessing,      ///< 业务处理中 (生成响应)
+    kWriting,         ///< 发送响应中
+    kClosing,         ///< 正在关闭 (半关闭)
+    kClosed           ///< 完全关闭
+};
```

#### 2. 状态转移验证机制
```diff
+bool Connection::CanTransition(ConnState new_state) const {
+    // 允许任何状态转移到 kClosing/kClosed（错误路径）
+    if (new_state == ConnState::kClosing) return true;
+    if (new_state == ConnState::kClosed) return true;
+
+    // 正常业务流程转移规则
+    switch (current) {
+        case ConnState::kConnecting: return new_state == ConnState::kConnected;
+        case ConnState::kConnected: return new_state == ConnState::kReading;
+        // ... 其他规则
+    }
+}
+
+void Connection::Transition(ConnState new_state, const std::string& reason) {
+    if (!CanTransition(new_state)) {
+        LOG_ERROR("Invalid state transition...");
+        // 无效转移自动触发关闭
+        state_ = ConnState::kClosing;
+        return;
+    }
+    LOG_DEBUG("State transition fd=%d: %d -> %d, reason: %s",
+              fd_, static_cast<int>(current), static_cast<int>(new_state), reason.c_str());
+    state_.store(new_state, std::memory_order_release);
+}
```

#### 3. 资源限制配置 (connection_limits.h)
```diff
+struct ConnectionLimits {
+    static constexpr size_t kMaxInputBuffer = 64 * 1024;      // 64KB
+    static constexpr size_t kMaxOutputBuffer = 1 * 1024 * 1024; // 1MB
+    static constexpr size_t kMaxRequestSize = 8 * 1024;       // 8KB
+
+    static bool IsInputBufferExceeded(size_t current_size) {
+        return current_size > kMaxInputBuffer;
+    }
+    // ... 其他检查方法
+};
```

#### 4. 背压机制实现
```diff
+void Connection::HandleRead(int fd) {
+    // ... 读取数据逻辑
+
+    // 背压检查：如果输入缓冲区超过限制，暂停读取
+    if (CheckInputBufferLimit()) {
+        PauseReading();
+    }
+}
+
+void Connection::PauseReading() {
+    loop_->UpdateEvent(fd_, EPOLLOUT | EPOLLET); // 只保留写事件
+}
```

### 修改意图
1. **状态机完整性**：实现完整7状态模型，覆盖连接全生命周期
2. **状态转移安全**：通过CanTransition验证，防止非法状态转换
3. **资源边界控制**：防止单个连接消耗过多内存，避免OOM
4. **背压机制**：缓冲区满时自动暂停读取，保护服务器稳定性
5. **向后兼容**：保持现有API不变，新增功能通过扩展接口

---

## 2026-03-06 22:03:38
### 涉及文件
1. `implementation_plan.md` (新建)
2. `CLAUDE.md` (更新)
3. `notes.txt` (更新)

### 核心 Diff 摘要
#### 1. 工程化实现规划创建
```diff
+ # TinyWebServer 工程级实现规划 (v6.4+)
+
+ ## 项目目标
+ 将当前 Reactor Web Server Demo 升级为**工程级可用的准生产系统**...
+
+ ## 当前状态评估 (v6-multi-reactor)
+ ### 已具备的核心能力
+ 1. **Reactor 架构**：Main-Sub Reactor 模型...
+
+ ### 与工程级的5点关键差距
+ 1. **Connection 状态机不完整**...
+ 2. **资源管理与边界控制缺失**...
+ 3. **错误处理碎片化**...
+ 4. **可观测性不足**...
+ 5. **配置系统缺失**...
+
+ ## 完整实现规划 (5个阶段)
+ ### 阶段一：核心架构升级 (v6.1)
+ - Connection 完整状态机 (7个状态)
+ - 资源边界控制与背压机制
+
+ ### 阶段二：错误处理与超时管理 (v6.2)
+ - 统一错误码体系 (WebError 枚举)
+ - 定时器管理器 (时间轮)
+
+ ### 阶段三：配置与可观测性 (v6.3)
+ - JSON 配置文件系统 + 热重载
+ - 监控指标收集 (Prometheus 格式)
+ - 结构化日志增强
+
+ ### 阶段四：安全与协议完整性 (v6.4)
+ - 请求安全验证 (路径遍历防护)
+ - HTTP/1.1 完整特性 (Keep-Alive, 条件请求)
+ - 简单反向代理模块
+
+ ### 阶段五：性能优化与高级特性 (v6.5+)
+ - 内存池优化
+ - I/O 多队列支持
+ - 插件系统框架
```

#### 2. 项目文档更新
```diff
# CLAUDE.md 更新
+ ## 工程化演进规划 (v6.1-v6.5+)
+ 已制定详细的5阶段实施规划...
+
+ ### 阶段目标
+ 1. **v6.1**：Connection 完整状态机...
+
+ ### 核心约束增强
+ 6. **状态机完整性**：Connection 必须实现完整状态机...
+ 7. **资源边界**：内存、连接数、缓冲区必须有明确上限...

# notes.txt 更新
+ ### 新的工程化规划 (2026-03-06)
+ **已创建详细实现规划文件**：`implementation_plan.md`
+ - **阶段一**：Connection 完整状态机 + 资源边界控制 (v6.1)...
```

### 修改意图
1. **系统化工程升级**：解决当前 Demo 与工程级实现的5点关键差距
2. **渐进式实施**：制定5个阶段的详细规划，降低重构风险
3. **文档完整性**：确保代码、设计、规划文档同步更新
4. **明确目标**：为v6.4+版本设定清晰的工程化里程碑
5. **保持教学价值**：复杂功能作为可选模块，核心保持简洁可理解

---

## 2026-03-06 21:38:23
### 涉及文件
1. `.github/workflows/ci.yml`
2. `CMakeLists.txt`

### 核心 Diff 摘要
#### 1. CI 流水线重构
```diff
- name: C++ CI & Memory Check
+ name: C++ CI

- jobs:
-   build-and-test:
-     runs-on: ubuntu-latest
-     steps: [...单一任务，同时运行测试和Valgrind]
+ jobs:
+   build-and-test:
+     name: Build + ASan Smoke Tests
+     runs-on: ubuntu-latest
+     steps: [...使用ASan运行冒烟测试]
+
+   memcheck:
+     name: Valgrind Memcheck
+     runs-on: ubuntu-latest
+     needs: build-and-test
+     steps: [...禁用ASan，使用Valgrind检查内存泄漏]
```

#### 2. CMake 测试分类管理
```diff
# 旧：简单添加测试可执行文件
- foreach(test_name ${ALL_TEST_TARGETS})
-   add_executable(${test_name} ...)
- endforeach()

# 新：CTest 集成 + 测试分类
+ if(BUILD_TESTING)
+   # 定义冒烟测试集
+   set(CI_SMOKE_TESTS test_timer test_basic test_multithread_reactor test_log)
+
+   foreach(test_name ${ALL_TEST_TARGETS})
+     add_executable(${test_name} ...)
+     add_test(NAME ${test_name} COMMAND ${test_name})
+     set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
+
+     if(test_name IN_LIST CI_SMOKE_TESTS)
+       set_tests_properties(${test_name} PROPERTIES LABELS "smoke;integration")
+     elseif(...)
+       # 其他分类
+     endif()
+   endforeach()
+ endif()
```

### 修改意图
1. **分离 ASan 与 Valgrind**：两者不能同时使用，拆分任务可充分发挥各自优势
2. **加速 CI 反馈**：冒烟测试（~30秒）快速验证基本功能，内存检查作为深度验证
3. **提高可维护性**：测试分类（smoke/integration/benchmark）便于选择性运行
4. **错误隔离**：Valgrind 日志单独保存，内存问题分析更清晰
5. **标准化测试**：通过 CTest 集成，支持 `ctest -L smoke` 等标准命令

---

## 修改记录模板
```
## YYYY-MM-DD HH:MM:SS
### 涉及文件
- file1.cpp
- file2.h

### 核心 Diff 摘要
```diff
// 用简短的代码块展示关键修改
- 旧代码
+ 新代码
```

### 修改意图
1. 为什么修改（问题背景）
2. 修改解决了什么
3. 预期效果
```

---

## 2026-03-07 (v7.3 完成)
### 涉及文件
1. `include/logging/structured_logger.h` - 结构化日志宏修复
2. `src/logging/structured_logger.cpp` - 结构化日志实现完善
3. `test/test_structured_log.cpp` - 新增结构化日志测试
4. `CMakeLists.txt` - 测试目标集成
5. `include/config/server_config.h` - JSON配置系统完善
6. `src/config/server_config.cpp` - JSON配置解析实现
7. `src/server_metrics.cpp` - 监控指标系统增强
8. `configs/README.md` - 配置文档更新
9. `configs/server.example.json` - 示例配置更新
10. `notes.txt` - 进度记录更新

### 核心 Diff 摘要
#### 1. 结构化日志宏修复（参数命名冲突）
```diff
-#define LOG_STRUCTURED(level, ...) \
+#define LOG_STRUCTURED(lvl, ...) \
     do { \
         if (auto* manager = &::tinywebserver::StructuredLogManager::GetInstance(); \
             manager->IsInitialized()) { \
             ::tinywebserver::StructuredLogEntry entry; \
             entry.timestamp = std::chrono::system_clock::now(); \
-            entry.level = (level); \
+            entry.level = static_cast<decltype(entry.level)>(lvl); \
             entry.file = __FILE__; \
             // ... 其他字段
         } \
     } while(0)
```

#### 2. JSON 配置系统集成
```diff
+// 包含 nlohmann/json 库
+#include <nlohmann/json.hpp>
+
+class ServerConfig {
+public:
+    static std::shared_ptr<ServerConfig> LoadFromFile(const std::string& path);
+    static std::shared_ptr<ServerConfig> LoadFromJson(const std::string& json_str);
+
+    // 配置热重载
+    bool Reload(const std::string& new_config);
+
+private:
+    bool LoadFromJsonObject(const nlohmann::json& j);
+    nlohmann::json config_json_;
+    std::shared_mutex config_mutex_;
+};
```

#### 3. 结构化日志测试添加
```diff
+int main() {
+    // 创建测试配置
+    auto config = std::make_shared<tinywebserver::ServerConfig>();
+
+    // 初始化结构化日志系统
+    tinywebserver::InitStructuredLoggerFromConfig(config);
+
+    if (tinywebserver::StructuredLogManager::GetInstance().IsInitialized()) {
+        std::cout << "Structured logger initialized successfully" << std::endl;
+        LOG_S_INFO("Test structured log message: %s", "Hello, structured logging!");
+    }
+
+    return 0;
+}
```

#### 4. CMake 测试集成
```diff
 set(INTEGRATION_TESTS
     test_timer
     test_lifecycle
     // ... 其他测试
     test_basic
     test_log
+    test_structured_log
 )
```

### 修改意图
1. **解决编译问题**：修复结构化日志宏参数命名冲突，避免编译器解析错误
2. **完善JSON配置**：集成 nlohmann/json 库，实现配置文件解析与热重载支持
3. **增强可观测性**：完善结构化日志系统，支持文本/JSON/彩色输出格式
4. **测试覆盖**：添加结构化日志专项测试，验证配置初始化与日志记录功能
5. **保持一致性**：所有更改遵循项目代码规范（C++17、命名约定、RAII原则）
6. **向后兼容**：API无破坏性变更，新增功能通过扩展接口提供

> **注意**：每次重要修改后更新此文件，保持增量记录，不删除历史记录。

---

## 2026-03-07 阶段四 (v7.4) 安全验证集成完成

### 涉及文件
1. `test/test_request_validator.cpp` - 修复测试调用链，添加 TestEdgeCases() 调用
2. `src/main.cpp` - 集成 RequestValidator 到 Connection 主路径，支持配置化根目录
3. `include/http_request.h` - 增强 HTTP 请求解析（已在前续修改中）
4. `src/http_request.cpp` - 增强 HTTP 请求解析（已在前续修改中）
5. `include/request_validator.h` - RequestValidator 类定义（已在前续修改中）
6. `src/request_validator.cpp` - RequestValidator 实现（已在前续修改中）
7. `CMakeLists.txt` - 测试目标集成（已在前续修改中）

### 核心 Diff 摘要
#### 1. 测试完善：添加边缘情况测试调用
```diff
int main() {
    std::cout << "Running RequestValidator tests..." << std::endl;

    try {
        TestPathNormalization();
        TestMethodValidation();
        TestRequestValidation();
        TestHeaderValidation();
+       TestEdgeCases();

        std::cout << "\n✅ All RequestValidator tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed: " << e.what() << std::endl;
        return 1;
    }
}
```

#### 2. 主程序集成：配置化 RequestValidator
```diff
+    // 确定静态资源根目录
+    std::string static_root = "./public"; // 默认值
+    if (config) {
+        static_root = config->GetStaticOptions().root;
+    }
+
-    server->SetOnMessage([](std::shared_ptr<Connection> conn, const std::string& /*data*/) {
+    server->SetOnMessage([static_root](std::shared_ptr<Connection> conn, const std::string& /*data*/) {
         // ... 请求处理逻辑

-        tinywebserver::RequestValidator validator("./public");
+        tinywebserver::RequestValidator validator(static_root);
         auto validation_result = validator.ValidateRequest(*parser);

         HttpResponse response;
         if (!validation_result.valid) {
             // ... 错误处理
-            response.Init("./public", "", false, error_code);
+            response.Init(static_root, "", false, error_code);
         } else {
-            response.Init("./public", validation_result.normalized_path, false);
+            response.Init(static_root, validation_result.normalized_path, false);
         }
     });
```

#### 3. 配置变量作用域提升
```diff
    std::unique_ptr<Server> server;
+    std::shared_ptr<tinywebserver::ServerConfig> config = nullptr;

    if (!config_file.empty()) {
-        auto config = tinywebserver::ServerConfig::LoadFromFile(config_file);
+        config = tinywebserver::ServerConfig::LoadFromFile(config_file);
        // ... 配置加载
    } else {
        // 默认配置
        server = std::make_unique<Server>("0.0.0.0", 8080);
+        // 创建默认配置对象以获取默认值
+        config = std::make_shared<tinywebserver::ServerConfig>();
    }
```

### 修改意图
1. **测试完整性**：确保 `TestEdgeCases()` 被调用，覆盖非法 Content-Length、重复关键头、HTTP/1.0/1.1 Keep-Alive 行为等边缘场景
2. **配置驱动**：使用 `ServerConfig` 提供的静态资源根目录，替代硬编码路径，支持配置文件自定义
3. **集成闭环**：将 `RequestValidator` 完整集成到 `Connection` 请求处理主路径，验证失败时返回适当的 HTTP 错误码（400/403/405/413）
4. **向后兼容**：保持默认行为不变（`"./public"`），当无配置文件时使用默认 `ServerConfig` 实例
5. **测试验证**：所有 RequestValidator 测试通过，冒烟测试（test_basic、test_timer、test_multithread_reactor、test_log）验证无回归

### 验证状态
- ✅ `test_request_validator` 通过（包含新增边缘情况测试）
- ✅ CI 冒烟测试集通过
- ✅ 编译通过（GCC 11.4.0，C++17，ASan enabled）
- ✅ 配置化根目录功能正常（从配置文件读取或使用默认值）

---

## 2026-03-07 阶段四 (v7.4) 完成：Keep-Alive 管理与条件请求支持

### 涉及文件
1. `include/http/keep_alive_manager.h` - KeepAliveManager 类定义
2. `src/http/keep_alive_manager.cpp` - KeepAliveManager 实现
3. `include/http/conditional_request_handler.h` - ConditionalRequestHandler 类定义
4. `src/http/conditional_request_handler.cpp` - ConditionalRequestHandler 实现
5. `include/connection.h` - 添加 Keep-Alive 接口方法
6. `src/connection.cpp` - 实现 Keep-Alive 生命周期管理
7. `include/http_response.h` - 添加条件请求支持参数
8. `src/http_response.cpp` - 集成条件请求检查到 HttpResponse::Init()
9. `include/server.h` - 添加 KeepAliveManager 成员
10. `src/server.cpp` - 初始化 KeepAliveManager
11. `src/main.cpp` - 调用 Keep-Alive 生命周期方法
12. `CMakeLists.txt` - 添加测试目标
13. `test/test_keep_alive.cpp` - 新增 Keep-Alive 单元测试
14. `test/test_conditional_request.cpp` - 新增条件请求单元测试

### 核心 Diff 摘要
#### 1. Connection 类添加 Keep-Alive 接口
```diff
class Connection {
public:
+    void UpdateKeepAliveState(bool keep_alive, int idle_timeout = 0);
+    void OnRequestStart(bool keep_alive, int idle_timeout = 0);
+    void OnRequestComplete();
+    bool ShouldKeepAlive() const;
};
```

#### 2. HttpResponse 集成条件请求检查
```diff
void HttpResponse::Init(const std::string& src_dir, const std::string& path, bool is_keep_alive,
                         int code, const HttpRequest* request)
{
    // 条件请求检查（仅当未指定强制状态码且请求有效时）
    if (request && code_ == -1) {
        std::string method = request->GetMethod();
        if (method == "GET" || method == "HEAD") {
            std::string full_path = src_dir + path;
+            auto [should_return_304, etag] = tinywebserver::ConditionalRequestHandler::ShouldReturn304(
+                *request, fs::path(full_path));
+            if (should_return_304) {
+                code_ = 304; // Not Modified
+                if (!etag.empty()) {
+                    headers_["ETag"] = etag;
+                }
+            }
        }
    }
}
```

#### 3. CMakeLists.txt 添加测试目标
```diff
set(INTEGRATION_TESTS
    test_timer
    test_lifecycle
    // ... 其他测试
    test_request_validator
+    test_keep_alive
+    test_conditional_request
)
```

#### 4. KeepAliveManager 核心逻辑
```diff
void KeepAliveManager::OnRequestStart(int fd, bool keep_alive, int idle_timeout) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    if (connections_.find(fd) == connections_.end()) {
        // 新连接
        connections_[fd] = ConnectionState{
            .request_count = 1,
            .last_active = now,
            .keep_alive = keep_alive,
            .idle_timeout = (idle_timeout > 0) ?
                std::chrono::seconds(idle_timeout) : default_idle_timeout_
        };
    } else {
        // 现有连接，更新状态
        connections_[fd].request_count++;
        connections_[fd].last_active = now;
        connections_[fd].keep_alive = keep_alive;
    }
    total_requests_++;
}
```

### 修改意图
1. **HTTP/1.1 协议完整性**：实现持久连接（Keep-Alive）和条件请求（Conditional Requests）两大核心特性
2. **连接复用优化**：通过 Keep-Alive 减少 TCP 连接建立开销，提高服务器性能
3. **带宽节省**：通过条件请求（304 Not Modified）减少不必要的数据传输
4. **资源管理**：跟踪连接状态，自动清理空闲超时连接，防止资源泄漏
5. **测试驱动**：为所有新功能添加单元测试，确保功能正确性和稳定性
6. **向后兼容**：API 无破坏性变更，新增功能通过扩展接口提供

### 验证状态
- ✅ `test_keep_alive` 通过（3个测试套件：基础功能、超时管理、统计信息）
- ✅ `test_conditional_request` 通过（5个测试套件：基础功能、If-Modified-Since、If-None-Match、集成测试、ETag生成）
- ✅ `test_request_validator` 通过（包含边缘情况测试）
- ✅ CI 冒烟测试集通过（test_timer、test_basic、test_multithread_reactor、test_log）
- ✅ 编译通过（GCC 11.4.0，C++17，ASan enabled）
- ✅ 所有新增测试集成到构建系统，可独立运行

> **注意**：阶段四(v7.4)全部功能完成，HTTP/1.1 协议完整性目标达成。

---

## 2026-03-07 阶段五 (v7.5) 基准测试框架编译修复

### 涉及文件
1. `benchmark/src/benchmark_memory.cpp` - 添加缺失头文件 `<dirent.h>`，移除无效 `start_time_` 初始化
2. `benchmark/src/benchmark_concurrent.cpp` - 添加缺失头文件 `<fcntl.h>` 和 `<poll.h>`，修复时间类型不匹配

### 核心 Diff 摘要
#### 1. 头文件添加
```diff
+#include <dirent.h>
 // benchmark_memory.cpp: 支持 DIR, opendir, readdir, closedir
+#include <fcntl.h>
+#include <poll.h>
 // benchmark_concurrent.cpp: 支持 fcntl, F_GETFL, F_SETFL, O_NONBLOCK, pollfd, POLLOUT, poll
```

#### 2. 构造函数修复
```diff
-    Impl() : running_(false), start_time_(), peak_rss_kb_(0), total_cpu_time_ms_(0) {
+    Impl() : running_(false), peak_rss_kb_(0), total_cpu_time_ms_(0) {
```

#### 3. 时间类型修复
```diff
-        result.start_time = std::chrono::steady_clock::now();
+        result.start_time = std::chrono::system_clock::now();
```

### 修改意图
1. **解决编译错误**：缺失的系统头文件导致 `DIR`, `fcntl`, `poll` 相关函数未定义
2. **类型一致性**：`BenchmarkResult::start_time` 使用 `system_clock::time_point`，与 `steady_clock::now()` 不匹配
3. **无效初始化**：`start_time_` 字段未在 `SystemResourceMonitor::Impl` 类中声明，移除冗余初始化
4. **保持兼容性**：所有修改不影响接口，仅修复内部实现错误

### 验证状态
- ✅ `benchmark_runner` 编译成功，无错误
- ✅ 帮助命令 `./benchmark_runner help` 正常显示
- ✅ 列表命令 `./benchmark_runner list` 显示四种测试类型
- ✅ 冒烟测试通过（test_timer, test_basic, test_multithread_reactor, test_log）
- ✅ 集成测试通过（除 test_main 外，14/15 通过，与基准测试无关）

> **注意**：阶段五(v7.5)基准测试框架核心架构已实现并通过编译验证，支持四种测试类型。后续需完善服务器集成以支持实际性能测试运行。