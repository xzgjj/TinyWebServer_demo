# CLAUDE.md - TinyWebServer 项目协作准则

## 项目简介
一个基于 Reactor 模式（epoll）的高性能 C++ Web Server 示例，专注于多 Reactor 架构、事件驱动模型与工程化代码结构。

## 构建与运行指令
### 编译
```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON ..
make -j$(nproc)
```

### 运行服务器
```bash
./build/server
```

### 运行测试
```bash
cd build
ctest --output-on-failure  # 运行所有测试
ctest -L smoke             # 仅运行冒烟测试
./test_basic               # 运行单个测试
```

## 代码规范
### 语言与标准
- C++17 标准
- 使用 `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion` 编译标志
- 头文件使用 `#pragma once`，禁止使用 `#include <bits/...>`
- 禁止使用 `using namespace std;` 在头文件中

### 命名习惯
- 类名、结构体名：`CamelCase`
- 函数名、变量名：`snake_case`
- 常量、枚举值：`UPPER_SNAKE_CASE`
- 私有成员变量：后缀 `_`（如 `fd_`）
- 类型别名：`CamelCase_t` 或 `CamelCasePtr`

### 必须使用的库与模式
- I/O 多路复用：epoll（边缘触发 ET 模式）
- 线程管理：pthread（通过 `std::thread` 封装）
- 内存管理：RAII 原则，智能指针（`std::unique_ptr`、`std::shared_ptr`）
- 资源管理：每个资源（fd、内存、锁）必须有明确的拥有者与生命周期

### 代码格式
- 缩进：4 个空格（禁止使用 Tab）
- 行宽：不超过 120 字符
- 花括号：K&R 风格（函数左括号换行，其他左括号不换行）
- 指针与引用：`Type* ptr`、`Type& ref`

## 常用命令
```bash
# 清理并重新构建
python3 tools.py clean
python3 tools.py all

# 调试特定测试
python3 tools.py debug --target test_backpressure

# 运行 CI 本地检查
python3 tools.py ci-local

# 内存检查（Valgrind）
python3 tools.py memcheck --target test_basic
```

## 核心约束
1. **一个 fd 只属于一个 EventLoop**：禁止跨线程迁移 fd，避免惊群与锁竞争
2. **Connection 是状态机**：状态转移必须显式定义，使用 `enum class` 标识状态
3. **非阻塞 I/O**：所有 socket 必须设置为 `O_NONBLOCK`，读写必须处理 `EAGAIN`/`EWOULDBLOCK`
4. **边缘触发（ET）**：accept、read、write 使用 ET 模式，必须循环读写直到返回 `EAGAIN`
5. **资源上限控制**：单个连接内存限制、日志队列容量、线程池大小必须有明确上限
6. **错误处理**：所有系统调用必须检查返回值，错误信息通过 `errno` 与项目错误码转换

## 核心逻辑速览
### 1. Reactor 架构
```
Acceptor (Main Reactor) → 监听端口，accept 新连接
       ↓
Sub Reactor 1..N (EventLoop + Thread) → 每个 Sub Reactor 管理一组 Connection
       ↓
Connection (状态机) → 处理 HTTP 请求/响应生命周期
```

### 2. 关键模块
- **EventLoop**：封装 `epoll_wait` + 事件回调派发，每个线程一个 EventLoop
- **Channel**：fd 的事件封装（EPOLLIN/EPOLLOUT + 回调函数）
- **Connection**：TCP 连接状态机（Connecting、Connected、Reading、Writing、Closing、Closed）
- **HttpParser**：HTTP 请求解析器（有限状态机）
- **AsyncLogger**：异步日志系统（有界队列 + 后台写线程）
- **ThreadPool**：Sub Reactor 线程池管理
- **TimerManager**：超时连接管理（时间轮或最小堆）

### 3. 数据流
```
客户端请求 → Acceptor → 分配 Sub Reactor → Connection::handleRead()
→ HttpParser::parse() → 生成 HttpResponse → Connection::handleWrite()
→ 发送响应 → 状态转移
```

## 安全与代码质量
### 编译期检查
- AddressSanitizer (ASan) 在 Debug 模式默认开启
- UndefinedBehaviorSanitizer (UBSan) 可选开启
- 静态分析：定期运行 `clang-tidy`

### 运行时检查
- 内存泄漏检测：Valgrind CI 流水线
- 边界检查：所有缓冲区操作必须验证大小
- 资源泄漏跟踪：fd、内存、锁的 RAII 包装

### 安全规范
- 所有用户输入（HTTP 请求）必须进行边界检查
- 文件路径必须规范化，防止目录遍历攻击
- 日志中禁止记录敏感信息（密码、令牌）
- 连接数限制防止 DoS 攻击

## AI 协作原则
### 修改调整范围
1. **允许修改**：
   - Bug 修复（内存泄漏、死锁、逻辑错误）
   - 性能优化（算法、数据结构、缓存）
   - 测试添加与完善（单元测试、集成测试）
   - 代码重构（提高可读性、降低复杂度）
   - 文档更新（注释、README、设计文档）

2. **禁止修改**：
   - 架构重大变更（如从 Reactor 切换到 Proactor）
   - 外部依赖库替换（除非存在安全漏洞）
   - 核心接口的破坏性变更

### 协作流程
1. **先读后写**：修改任何文件前必须先阅读现有实现，理解设计意图
2. **小步提交**：每个提交只解决一个问题，提交信息清晰说明“为什么改”
3. **同步更新**：每次 Git Commit 后必须更新 `notes.txt` 与 `diff.md`
4. **测试通过**：修改后必须通过现有测试，新增功能必须添加测试
5. **风格一致**：保持现有代码风格，不引入个人偏好

### DeepSeek R1 推理接口特别说明
- 如果集成 DeepSeek R1 推理接口，注意处理思考 Token 的输出
- AI 相关代码应单独模块化，不污染核心网络逻辑
- 推理服务应作为可选插件，通过配置开关启用

---

## 工程化演进规划 (v6.1-v6.5+)
已制定详细的5阶段实施规划，详见 `implementation_plan.md`：

### 阶段目标
1. **v6.1**：Connection 完整状态机 (7个状态) + 资源边界控制
2. **v6.2**：统一错误码体系 + 定时器管理器
3. **v6.3**：JSON 配置系统 + 监控指标 + 结构化日志
4. **v6.4**：请求安全验证 + HTTP/1.1 协议完整性
5. **v6.5+**：性能优化 + 插件系统 + 高级特性

### 实施原则
- **向后兼容**：保持现有 API 不变，新增功能通过扩展接口
- **渐进式升级**：每个阶段独立可测试，风险可控
- **教学价值优先**：复杂功能作为可选模块，核心保持简洁
- **测试驱动**：每个功能必须附带单元测试和集成测试

### 核心约束增强
6. **状态机完整性**：Connection 必须实现完整状态机 (7个状态)
7. **资源边界**：内存、连接数、缓冲区必须有明确上限
8. **错误处理统一**：所有错误路径必须使用项目级错误码
9. **配置外部化**：所有参数必须支持配置文件热重载
10. **可观测性**：必须提供完整监控指标和结构化日志

---

**最后更新**：2026-03-06
**维护者**：Claude Code
**项目阶段**：v6-engineering（工程化升级期）
**当前重点**：阶段一 - Connection 状态机与资源边界控制