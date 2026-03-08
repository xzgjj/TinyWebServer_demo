# TinyWebServer 工程级实现规划 



## 项目目标
将当前 Reactor Web Server Demo 升级为**工程级可用的准生产系统**，同时保持代码清晰、架构明确的教学价值。



## 当前状态评估 (v6-multi-reactor)



### 已具备的核心能力
1. **Reactor 架构**：Main-Sub Reactor 模型，EventLoop + Channel 事件驱动
2. **零拷贝优化**：BufferChain 支持 writev 聚集写，mmap 文件映射
3. **异步日志**：双缓冲异步日志系统
4. **HTTP 基础**：请求解析、静态资源服务
5. **工程化基础**：CMake 构建、测试框架、CI/CD 流水线



### 与工程级的5点关键差距
1. **Connection 状态机不完整**：仅4个基础状态，缺少业务状态（READING/WRITING/PROCESSING）
2. **资源管理与边界控制缺失**：无内存上限、无背压机制、日志队列无限增长
3. **错误处理碎片化**：系统 errno 与项目错误码混用，关闭路径不一致
4. **可观测性不足**：缺少运行时指标（连接数、QPS、延迟分布）
5. **配置系统缺失**：硬编码参数，无法热重载



## 完整实现规划



### 阶段一：核心架构升级 (v6.1)
**目标**：解决 Connection 状态机和资源边界问题

#### 1.1 Connection 完整状态机
```cpp
// 当前状态 (4个)
enum class ConnState { kConnecting, kConnected, kDisconnecting, kDisconnected };

// 目标状态 (7个 + 子状态)
enum class ConnState {
    kConnecting,      // 连接建立中 (SYN_SENT)
    kConnected,       // 连接已建立，等待请求
    kReading,         // 读取请求中
    kProcessing,      // 业务处理中 (生成响应)
    kWriting,         // 发送响应中
    kClosing,         // 正在关闭 (半关闭)
    kClosed           // 完全关闭
};

// 状态转移表
class ConnectionStateMachine {
    bool CanTransition(ConnState from, ConnState to);
    void Transition(ConnState new_state, const std::string& reason);

private:
    std::atomic<ConnState> current_state_;
    std::unordered_map<ConnState, std::set<ConnState>> transition_table_;
};
```

#### 1.2 资源边界控制
```cpp
// 每个 Connection 的资源限制
struct ConnectionLimits {
    static constexpr size_t kMaxInputBuffer = 64 * 1024;     // 64KB 读缓冲区
    static constexpr size_t kMaxOutputBuffer = 1 * 1024 * 1024;  // 1MB 写缓冲区
    static constexpr int kMaxRequestSize = 8 * 1024;        // 8KB 单个请求
    static constexpr int kMaxHeadersSize = 8 * 1024;        // 8KB 头部

    bool CheckBufferLimit(const Connection& conn) const;
};

// 全局资源限制
struct GlobalLimits {
    static constexpr int kMaxConnections = 10000;           // 最大连接数
    static constexpr int kMaxFds = 10240;                   // 最大文件描述符
    static constexpr size_t kTotalMemoryLimit = 256 * 1024 * 1024; // 256MB 总内存
};

// 异步日志有界队列
class BoundedAsyncLogger : public AsyncLogger {
    static constexpr size_t kMaxPendingLogs = 10000;        // 队列上限
    enum class FullPolicy { BLOCK, DISCARD_OLDEST, DISCARD_NEWEST };
};
```

#### 1.3 实现任务清单
- [ ] 重构 Connection 类，添加完整状态枚举
- [ ] 实现状态转移表与状态机逻辑
- [ ] 为 Connection 添加资源限制检查
- [ ] 实现异步日志队列容量限制
- [ ] 添加背压机制：缓冲区满时暂停读取
- [ ] 更新所有相关测试用例



### 阶段二：错误处理与超时管理 (v6.2)
**目标**：统一错误处理体系，添加连接超时

#### 2.1 项目级错误码体系
```cpp
// 统一错误码（兼容系统 errno）
enum class WebError {
    kSuccess = 0,
    kSocketError,           // socket 系统调用失败
    kEpollError,            // epoll 操作失败
    kBufferFull,            // 缓冲区已满
    kParseError,            // HTTP 解析失败
    kTimeout,               // 操作超时
    kResourceNotFound,      // 静态资源不存在
    kAccessDenied,          // 访问被拒绝
    kConnectionLimit,       // 连接数超限
    kMemoryLimit,           // 内存不足
    kProtocolError,         // 协议错误
    kInternalError          // 内部逻辑错误
};

// 错误包装类
class Error {
public:
    Error(WebError code, const std::string& message, int sys_errno = 0);
    std::string ToString() const;
    bool IsSuccess() const { return code_ == WebError::kSuccess; }

private:
    WebError code_;
    std::string message_;
    int sys_errno_;
    std::chrono::system_clock::time_point timestamp_;
};

// 错误处理宏
#define RETURN_IF_ERROR(expr) \
    do { \
        auto _result = (expr); \
        if (!_result.IsSuccess()) return _result; \
    } while(0)
```

#### 2.2 定时器管理器
```cpp
// 简易时间轮（1秒精度）
class TimerWheel {
public:
    struct TimerTask {
        int fd;
        std::function<void()> callback;
        int remaining_ticks;
    };

    void AddTimeout(int fd, int seconds, std::function<void()> callback);
    void RemoveTimeout(int fd);
    void Tick();  // 每秒调用一次

private:
    std::vector<std::list<TimerTask>> wheel_;
    std::unordered_map<int, std::list<TimerTask>::iterator> timers_;
    int current_slot_;
};

// Connection 超时配置
struct TimeoutConfig {
    static constexpr int kConnectTimeout = 5;      // 连接建立超时
    static constexpr int kReadTimeout = 30;        // 读超时
    static constexpr int kWriteTimeout = 10;       // 写超时
    static constexpr int kKeepAliveTimeout = 15;   // Keep-Alive 空闲超时
};
```

#### 2.2 统一关闭路径
```cpp
class Connection {
    // 统一的关闭入口
    void Close(Error reason);

private:
    // 内部关闭实现
    void CloseInLoop(Error reason);

    // 关闭步骤
    void ShutdownWrite();
    void DrainOutputBuffer();
    void CleanupResources();
    void NotifyClose();
};
```

#### 2.3 实现任务清单
- [ ] 定义 WebError 枚举和 Error 类
- [ ] 替换所有 errno 使用为 Error 对象
- [ ] 实现 TimerWheel 时间轮
- [ ] 为 Connection 集成超时管理
- [ ] 实现统一关闭路径
- [ ] 添加错误处理测试用例



### 阶段三：配置与可观测性 (v6.3)
**目标**：参数外部化，添加基础监控

#### 3.1 JSON 配置文件系统
```json
// config/server.json
{
    "server": {
        "ip": "0.0.0.0",
        "port": 8080,
        "threads": 4,
        "backlog": 1024,
        "tcp_nodelay": true,
        "tcp_cork": false
    },
    "limits": {
        "max_connections": 10000,
        "max_request_size": 65536,
        "max_response_size": 1048576,
        "connection_timeout": 30,
        "keep_alive_timeout": 15
    },
    "logging": {
        "level": "INFO",
        "file": "logs/server.log",
        "async": true,
        "queue_size": 10000,
        "flush_interval": 3
    },
    "static": {
        "root": "./www",
        "cache_size": 100,
        "cache_ttl": 300
    }
}
```

```cpp
// 配置类
class ServerConfig {
public:
    static ServerConfig LoadFromFile(const std::string& path);
    static ServerConfig LoadFromJson(const std::string& json_str);

    // 热重载支持
    bool Reload(const std::string& new_config);
    bool CanReloadWithoutRestart() const;

    // 配置验证
    std::vector<std::string> Validate() const;

private:
    std::atomic<bool> reloading_;
    std::shared_mutex config_mutex_;
    nlohmann::json config_json_;
};
```

#### 3.2 监控指标系统
```cpp
// 指标收集器
class MetricsCollector {
public:
    // 连接层指标
    struct ConnectionMetrics {
        std::atomic<int64_t> total_accepted;
        std::atomic<int64_t> current_active;
        std::atomic<int64_t> total_closed;
        std::atomic<int64_t> rejected_limits;
        histogram_t duration_histogram;
    };

    // 请求层指标
    struct RequestMetrics {
        std::atomic<int64_t> total_requests;
        std::atomic<int64_t> requests_1xx;
        std::atomic<int64_t> requests_2xx;
        std::atomic<int64_t> requests_3xx;
        std::atomic<int64_t> requests_4xx;
        std::atomic<int64_t> requests_5xx;
        histogram_t latency_histogram;
    };

    // 资源层指标
    struct ResourceMetrics {
        std::atomic<int64_t> memory_used;
        std::atomic<int64_t> memory_allocated;
        std::atomic<int64_t> fds_used;
        std::atomic<int64_t> epoll_wait_time_us;
    };

    // 指标导出
    std::string ExportPrometheusFormat() const;
    std::string ExportJsonFormat() const;
    void Reset();  // 用于测试

    // 单例访问
    static MetricsCollector& GetInstance();
};
```

#### 3.3 结构化日志
```cpp
// 结构化日志条目
struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string message;
    std::string file;
    int line;
    std::string function;

    // 请求上下文（如果可用）
    std::optional<std::string> request_id;
    std::optional<int> connection_id;
    std::optional<std::string> client_ip;

    // 转换为不同格式
    std::string ToText() const;
    std::string ToJson() const;
};

// 增强的日志宏
#define LOG_STRUCTURED(level, ...) \
    do { \
        LogEntry entry; \
        entry.timestamp = std::chrono::system_clock::now(); \
        entry.level = level; \
        entry.file = __FILE__; \
        entry.line = __LINE__; \
        entry.function = __func__; \
        /* 填充请求上下文 */ \
        Logger::GetInstance().Log(entry); \
    } while(0)
```

#### 3.3 实现任务清单
- [ ] 集成 JSON 解析库（nlohmann/json）
- [ ] 实现 ServerConfig 配置类
- [ ] 实现配置热重载机制
- [ ] 实现 MetricsCollector 指标系统
- [ ] 添加 Prometheus 格式导出
- [ ] 增强日志系统支持结构化日志
- [ ] 添加配置验证和默认值
- [ ] 创建示例配置文件



### 阶段四：安全与协议完整性 (v7.4)
**目标**：添加基础安全防护，完善 HTTP/1.1 协议；支持 HTTP/2 基础特性

#### 4.1 请求安全验证
```cpp
class RequestValidator {
public:
    struct ValidationResult {
        bool valid;
        Error error;
        std::string normalized_path;
    };

    ValidationResult ValidateRequest(const HttpRequest& request);

private:
    // 路径安全检查
    bool IsPathSafe(const std::string& path);
    std::string NormalizePath(const std::string& path);

    // 头部安全检查
    bool AreHeadersSafe(const HttpHeaders& headers);

    // 请求大小检查
    bool IsRequestSizeValid(size_t content_length, size_t headers_size);

    // 方法白名单
    static const std::set<std::string> kAllowedMethods;
};
```

#### 4.2 HTTP/1.1 核心特性
```cpp
// Keep-Alive 连接管理
class KeepAliveManager {
public:
    void OnRequestStart(int fd);
    void OnRequestComplete(int fd);
    void OnIdleTimeout(int fd);

    struct ConnectionState {
        int request_count;
        std::chrono::steady_clock::time_point last_active;
        bool keep_alive;
    };

private:
    std::unordered_map<int, ConnectionState> connections_;
    std::mutex mutex_;
};

// 条件请求支持
class ConditionalRequestHandler {
public:
    bool ShouldReturn304(const HttpRequest& request,
                        const FileStat& file_stat,
                        std::string& etag);

private:
    std::string GenerateETag(const FileStat& stat);
    bool CheckIfModifiedSince(const HttpRequest& request,
                             const FileStat& stat);
    bool CheckIfNoneMatch(const HttpRequest& request,
                         const std::string& etag);
};
```

#### 4.3 HTTP/2 协议支持
```cpp
// HTTP/2 帧类型
enum class H2FrameType : uint8_t {
    DATA = 0x0,
    HEADERS = 0x1,
    PRIORITY = 0x2,
    RST_STREAM = 0x3,
    SETTINGS = 0x4,
    PUSH_PROMISE = 0x5,
    PING = 0x6,
    GOAWAY = 0x7,
    WINDOW_UPDATE = 0x8,
    CONTINUATION = 0x9
};

// HTTP/2 帧头（9字节）
struct H2FrameHeader {
    uint32_t length : 24;
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id : 31;
    uint8_t reserved : 1;
};

// HTTP/2 连接状态机
class H2Connection {
public:
    enum class State {
        H2_IDLE,           // 等待客户端前言
        H2_PREFACE_SENT,   // 已发送服务器前言
        H2_OPEN,           // 连接已打开
        H2_CLOSING,        // 正在关闭 (GOAWAY 已发送)
        H2_CLOSED          // 连接已关闭
    };

    // 处理接收到的帧
    Error HandleFrame(const H2FrameHeader& header, const uint8_t* payload);

    // 发送帧
    Error SendFrame(H2FrameType type, uint8_t flags, uint32_t stream_id,
                    const uint8_t* payload, size_t length);

private:
    std::unordered_map<uint32_t, H2Stream> streams_;
    State state_;
    H2Settings settings_;
    HpackDecoder hpack_decoder_;
    HpackEncoder hpack_encoder_;
};

// HTTP/2 流状态机 (每个请求/响应流)
class H2Stream {
public:
    enum class State {
        IDLE,
        RESERVED_LOCAL,
        RESERVED_REMOTE,
        OPEN,
        HALF_CLOSED_LOCAL,
        HALF_CLOSED_REMOTE,
        CLOSED
    };

    State state() const { return state_; }
    Error HandleHeaders(const std::vector<HpackHeader>& headers, bool end_stream);
    Error HandleData(const uint8_t* data, size_t len, bool end_stream);

private:
    State state_;
    uint32_t stream_id_;
    std::vector<HpackHeader> request_headers_;
    std::vector<uint8_t> request_body_;
    // 响应相关
};
```

#### 4.4 简单反向代理
```cpp
// 上游服务器配置
struct UpstreamServer {
    std::string host;
    int port;
    int weight;
    bool healthy;
    std::chrono::steady_clock::time_point last_check;
};

// 负载均衡策略
class LoadBalancer {
public:
    enum class Strategy { ROUND_ROBIN, LEAST_CONNECTIONS, IP_HASH };

    std::optional<UpstreamServer> SelectUpstream(const std::string& client_ip);
    void MarkUpstreamStatus(const std::string& host, int port, bool healthy);

private:
    std::vector<UpstreamServer> upstreams_;
    Strategy strategy_;
    std::atomic<size_t> round_robin_index_;
};

// 反向代理处理器
class ReverseProxyHandler {
public:
    Response HandleRequest(const Request& request);

private:
    std::shared_ptr<Connection> CreateUpstreamConnection(const UpstreamServer& server);
    void ForwardRequest(std::shared_ptr<Connection> client_conn,
                       std::shared_ptr<Connection> upstream_conn);
    void HandleUpstreamResponse(std::shared_ptr<Connection> client_conn,
                               std::shared_ptr<Connection> upstream_conn);
};
```

#### 4.5 实现任务清单
- [ ] 实现 RequestValidator 请求验证器
- [ ] 添加路径规范化与安全检查
- [ ] 实现 KeepAliveManager 连接复用管理
- [ ] 添加条件请求支持（If-Modified-Since, ETag）
- [ ] 实现简单反向代理模块
- [ ] 添加负载均衡基础策略
- [ ] 创建安全测试用例
- [ ] 更新 HTTP 协议测试
- [ ] 实现 HTTP/2 帧解析器（基础帧头解析）
- [ ] 实现 HTTP/2 连接状态机（H2Connection）
- [ ] 实现 HTTP/2 流状态机（H2Stream）
- [ ] 实现 HPACK 头部压缩解码器（基础实现）
- [ ] 添加 HTTP/2 协议升级协商（h2c）
- [ ] 集成 HTTP/2 到 Connection 状态机
- [ ] 创建 HTTP/2 测试用例



### 阶段五：性能优化与高级特性 (v7.5+)
**目标**：性能深度优化，添加高级特性（可选）

#### 5.1 内存池优化
```cpp
// 分级内存池
class MemoryPool {
public:
    enum class SizeClass {
        SMALL = 64,      // <= 64B
        MEDIUM = 256,    // <= 256B
        LARGE = 1024,    // <= 1KB
        HUGE             // > 1KB，直接 malloc
    };

    void* Allocate(size_t size);
    void Deallocate(void* ptr, size_t size);

private:
    struct Slab {
        void* memory;
        size_t block_size;
        std::vector<bool> used;
    };

    std::unordered_map<SizeClass, std::vector<Slab>> slabs_;
    std::mutex mutex_;
};
```

#### 5.2 I/O 多队列优化
```cpp
// SO_REUSEPORT 支持
class MultiListenSocket {
public:
    bool BindAndListen(const std::string& ip, int port, int num_queues);

    // 每个线程获取自己的监听 socket
    int GetListenFdForThread(int thread_id);

private:
    std::vector<int> listen_fds_;
    std::mutex mutex_;
};

// I/O 批处理
class BatchIOHandler {
public:
    void ProcessBatch(const std::vector<epoll_event>& events);

private:
    void BatchRead(const std::vector<int>& readable_fds);
    void BatchWrite(const std::vector<int>& writable_fds);
    void BatchError(const std::vector<int>& error_fds);
};
```

#### 5.3 插件系统框架
```cpp
// 插件接口
class Plugin {
public:
    virtual ~Plugin() = default;
    virtual std::string Name() const = 0;
    virtual std::string Version() const = 0;

    virtual bool OnLoad(Server& server) = 0;
    virtual bool OnUnload() = 0;

    virtual void OnRequestStart(Request& request) {}
    virtual void OnRequestComplete(Request& request, Response& response) {}
    virtual void OnConnectionOpen(int fd) {}
    virtual void OnConnectionClose(int fd) {}
};

// 插件管理器
class PluginManager {
public:
    bool LoadPlugin(const std::string& path);
    bool UnloadPlugin(const std::string& name);

    void NotifyRequestStart(Request& request);
    void NotifyRequestComplete(Request& request, Response& response);

private:
    std::unordered_map<std::string, std::unique_ptr<Plugin>> plugins_;
    std::vector<Plugin*> request_start_chain_;
    std::vector<Plugin*> request_complete_chain_;
};
```

#### 5.4 实现任务清单
- [x] 建立性能基准测试框架 (基础框架 + 最小验证修复)
- [x] 实现分级内存池 (MemoryPool 完整实现)
- [x] 添加 SO_REUSEPORT 多监听支持 (MultiListenSocket 完整实现)
- [x] 实现 I/O 批处理优化 (BatchIOHandler 完整实现)
- [x] 设计插件系统接口 (Plugin 接口完整定义)
- [x] 实现插件加载机制 (PluginManager 单例管理)
- [x] 创建示例插件 (ExamplePlugin 演示插件)
- [x] 性能基准测试套件 (QPS/延迟/内存/并发四种测试)

#### 5.4完善 正在进行
- [x] CI/CD 集成完善 (将性能回归检测集成到 CI 流程)
- [x] 性能回归检测机制 (compare_benchmark.py 脚本集成)
- [ ] 结果可视化增强 (改进 JSON/CSV 导出格式)
- [ ] 压力测试场景扩展 (添加更多真实负载模式)
- [ ] 测试完整性修复 (解决 gtest 依赖问题)



## 实施优先级与时间规划



### 优先级划分
| 优先级 | 阶段 | 核心价值 | 预计工作量 |
|--------|------|----------|-----------|
| **P0** | 阶段一 + 阶段二 | 架构完整性、稳定性基础 | 2-3周 |
| **P1** | 阶段三 | 可维护性、可观测性 | 1-2周 |
| **P2** | 阶段四 | 安全性、协议完整性 | 2-3周 |
| **P3** | 阶段五 | 性能优化、扩展性 | 3-4周 |



### 里程碑计划



**Milestone 1 (v6.1): 状态机与资源边界** - 2周
- Connection 完整状态机
- 资源限制与背压机制
- 错误码体系基础

**Milestone 2 (v6.2): 错误处理与超时** - 1周
- 统一错误处理
- 定时器管理器
- 连接超时控制

**Milestone 3 (v6.3): 配置与监控** - 2周
- JSON 配置文件系统
- 指标收集与导出
- 结构化日志增强

**Milestone 4 (v6.4): 安全与协议** - 2周
- 请求安全验证
- HTTP/1.1 Keep-Alive
- 条件请求支持

**Milestone 5 (v6.5): 反向代理** - 2周
- 简单反向代理实现
- 负载均衡策略
- 上游健康检查

**Milestone 6 (v6.6+): 高级优化** - 3-4周
- 内存池优化
- I/O 多队列支持
- 插件系统框架



## 测试策略



### 单元测试覆盖
1. **状态机测试**：验证所有状态转移路径
2. **资源限制测试**：边界条件、OOM 场景模拟
3. **错误处理测试**：各种错误场景恢复
4. **配置解析测试**：配置文件格式验证



### 集成测试
1. **协议合规性测试**：使用 h2spec 等工具验证 HTTP 协议
2. **压力测试**：ab/wrk 进行并发连接测试
3. **内存泄漏测试**：Valgrind + ASan 持续检测
4. **故障恢复测试**：模拟网络异常、进程崩溃



### 性能基准
1. **建立性能基线**：QPS、延迟、内存使用
2. **回归测试**：每次提交对比性能变化
3. **硬件适配测试**：不同 CPU 架构、网卡型号



## 代码组织与架构调整



### 目录结构调整建议
```
TinyWebServer_demo/
├── src/
│   ├── core/           # 核心框架
│   │   ├── connection/
│   │   ├── eventloop/
│   │   ├── timer/
│   │   └── buffer/
│   ├── http/          # HTTP 协议实现
│   │   ├── parser/
│   │   ├── handler/
│   │   └── validator/
│   ├── config/        # 配置系统
│   ├── metrics/       # 监控指标
│   ├── utils/         # 工具类
│   └── plugins/       # 插件系统（可选）
├── include/           # 对应头文件
├── tests/            # 测试代码
│   ├── unit/         # 单元测试
│   ├── integration/  # 集成测试
│   └── benchmarks/   # 性能测试
├── configs/          # 配置文件示例
├── scripts/          # 构建/部署脚本
└── docs/             # 设计文档
```



### 关键设计决策



1. **向后兼容性**：保持现有 API 不变，新增功能通过扩展接口
2. **编译时开关**：高级特性通过 CMake 选项控制
3. **依赖管理**：第三方库（JSON 解析）作为子模块或条件引入
4. **ABI 稳定性**：核心接口使用稳定版本号



## 风险管理与缓解



### 技术风险
1. **状态机复杂度**：可能导致死锁或状态不一致
   - 缓解：严格的状态转移验证，状态机单元测试覆盖
2. **内存池碎片**：长期运行可能导致性能下降
   - 缓解：定期内存整理，支持切换回系统分配器
3. **热重载竞争条件**：配置更新时可能引发数据竞争
   - 缓解：读写锁保护，原子配置切换
   
   

### 项目风险
1. **范围蔓延**：功能过多失去 Demo 的简洁性
   - 缓解：严格遵循优先级，P3 特性作为可选模块
2. **测试覆盖率下降**：新功能增加测试难度
   - 缓解：测试驱动开发，保持 >80% 覆盖率
3. **文档滞后**：实现快于文档更新
   - 缓解：每个功能必须附带设计文档和 API 文档
   
   

## 成功标准



### 功能完成度
- [ ] Connection 完整状态机实现并通过测试
- [ ] 资源边界控制有效，防止 OOM
- [ ] 统一错误处理覆盖所有路径
- [ ] 配置系统支持热重载
- [ ] 监控指标完整，可 Prometheus 导出
- [ ] 基础安全防护（路径遍历、请求限制）
- [ ] HTTP/1.1 Keep-Alive 和条件请求支持



### 质量指标
- [ ] 内存泄漏为零（Valgrind 验证）
- [ ] 测试覆盖率 >80%
- [ ] 通过压力测试（10k 并发连接）
- [ ] 代码复杂度保持合理（圈复杂度 <15）
- [ ] 编译警告为零（-Wall -Wextra -Werror）



### 工程化指标
- [ ] 完整的设计文档
- [ ] 清晰的 API 文档
- [ ] 示例配置文件
- [ ] 性能基准报告
- [ ] 部署指南



## 开始执行的准备



### 立即行动项
1. **创建分支**：`git checkout -b v6-engineering`
2. **更新 CLAUDE.md**：反映新的工程化目标
3. **设置开发环境**：确保测试框架、代码分析工具就绪
4. **建立性能基线**：记录当前版本的性能指标



### 开发流程
1. **小步提交**：每个功能独立提交，包含测试
2. **代码审查**：通过 CI 自动检查 + 人工审查
3. **文档同步**：代码与文档同步更新
4. **回归测试**：确保已有功能不受影响

---

**下一步**：按照本规划开始阶段一实施，首先重构 Connection 状态机。每个阶段完成后进行评审，确保方向正确。

**最后更新**：2026-03-06
**版本**：v1.0 规划草案