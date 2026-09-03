# TinyWebServer 架构总览与数据流（结对审计基线）

- **生成时间:** 2026-09-03
- **审计单元:** [audit-w1] docs: 架构总览与数据流
- **方式:** 通读主线代码后的独立理解整理（对照: `README.md` / `CLAUDE.md` / `notes.txt` / `doc/v7.md`）
- **状态:** 待结对审核（疑问清单见文末第 9 节）

---

## 1. 一句话总览

Main/Sub Reactor 模型：主线程跑 Main EventLoop（传统模式负责 accept 分发），N 个 I/O 线程各跑一个 Sub EventLoop（One Loop Per Thread），每个连接从建立到销毁**只属于一个 EventLoop**；HTTP 解析、校验、响应生成全部在 I/O 线程内**同步**完成，发送走零拷贝/异步路径。

---

## 2. 两种运行模式（构造时二选一，`Server` 构造函数内切换）

```text
传统模式：                           SO_REUSEPORT 模式：
Main Reactor (epoll, accept)          Sub Reactor 1 ── listen socket 1 ── conn 1..N
   │ round-robin 分发                  Sub Reactor 2 ── listen socket 2 ── conn 1..N
   ▼                                  ...（内核级负载均衡，无中央 accept）
Sub Reactor 1..N                      （每个 Sub Reactor 直接 accept 自己 socket 的连接）
(epoll_wait + Connection 状态机)
```

| 维度 | 传统模式 | SO_REUSEPORT 模式 |
| :--- | :--- | :--- |
| 监听 socket | 单监听（`CreateListenSocket`） | N 个（`MultiListenSocket`，内核负载均衡） |
| accept 位置 | Main Reactor（主线程） | 各 Sub Reactor（各自线程内） |
| 连接分配 | `GetNextLoop()` round-robin（next_++ % size） | 无分配，谁 accept 谁负责 |
| 主 Reactor 职责 | 监听 + accept + 分发 | 不监听任何 socket（纯管理） |
| 配置开关 | `use_so_reuseport: false`（默认） | `use_so_reuseport: true` + `so_reuseport_sockets` |

两个 socket 均带 `SOCK_NONBLOCK | SOCK_CLOEXEC`；监听 socket 设 `SO_REUSEADDR`，SO_REUSEPORT 模式另加 `SO_REUSEPORT`（需内核 3.9+）。

---

## 3. 启动流程（src/main.cpp）

1. **环境收敛**：兜底创建 `public/`、`local/logs/`（含 benchmark 子目录），默认 `public/index.html`（`<h1>TinyWebServer V3</h1>`）
2. **日志初始化**：`Logger::GetInstance().Init(local/logs/tiny_server.log, LOG_LEVEL_INFO)`；若有配置，再 `InitStructuredLoggerFromConfig(config)`
3. **Server 构造**：有 `--config` 则 JSON 配置加载（失败即退出），否则默认 `0.0.0.0:8080`；构造函数内按 `use_so_reuseport` 完成模式安装（传统模式此时只创建监听 socket，事件注册延后到 Run()）
4. **插件**：`RegisterPlugin<ExamplePlugin>()` → `LoadPlugins()`（`plugin_manager.LoadAllPlugins(*server)`）
5. **注册业务回调**：`SetOnMessage(...)` —— 这是 HTTP 链路的核心（见第 7 节），回调捕获 `static_root` 与 `keep_alive_timeout`
6. **启动**：`Start()`（真正启动 N 个 Sub Loop 线程）→ `Run()`（传统模式此时才把 `listen_fd` 注册进 main_loop：`EPOLLIN | EPOLLET`，然后 `main_loop_->Loop()`）

参数：`--config <path>` / `--help`，其余参数视为未知并退出。

---

## 4. 线程模型与 fd 归属

### 4.1 线程构成

| 线程 | 角色 | 说明 |
| :--- | :--- | :--- |
| 主线程 | Main EventLoop | 传统模式 accept；SO_REUSEPORT 模式下不监听 |
| I/O 线程 × N | Sub EventLoop × N | `EventLoopThread::StartLoop()` 线程启动即 `Loop()`；默认 N=配置 `threads` |

### 4.2 fd 归属纪律（关键设计）

1. `HandleAccept`（主线程）只做：accept（循环到 EAGAIN）→ `make_shared<Connection>(conn_fd, io_loop, ...)`（初始状态 `kConnecting`）→ 写入 `connections_`（`mutex` 保护，因 `RemoveConnection` 会从其他线程调用）→ `io_loop->RunInLoop([conn]{ conn->ConnectEstablished(); })`
2. `ConnectEstablished` **必须在 loop 线程**（内有 `LOG_FATAL` 断言）——此后该 fd 的所有 epoll 操作、读/写/关都在该 loop 线程串行执行
3. 跨线程访问的通道：
   - `RunInLoop / QueueInLoop`：eventfd 唤醒 + `pending_functors_` 交换队列（锁内只 swap，执行在锁外）
   - `UpdateEvent`：非 loop 线程调用时自动 `RunInLoop` 派发（保证 epoll_ctl 单线程串行）
   - `Send / Shutdown / Close`：非 loop 线程时包装 `RunInLoop`（线程安全接口）

### 4.3 EventLoopThreadPool

`GetNextLoop()`：`loops_.empty() ? base_loop_ : loops_[next_++ % size]`（纯轮询，无负载信息）。

---

## 5. 事件循环（EventLoop::Loop，每线程一个）

```text
while (!quit_) {
    ProcessTimers();                 // TimerWheel.Tick()（1s 精度，无独立定时器线程）
    next_timeout = 定时器最近到期时间（无→100ms，有→min(到期ms, 1000)）;
    epoll_wait(..., next_timeout);   // 单次最多 kMaxEvents=1024
    BatchIOHandler.ProcessBatch();   // error > read > write 分组 → 逐组回调
    DoPendingFunctors();             // 跨线程任务（swap 后执行）
}
```

- **无 Channel 类**：回调存储 = `unordered_map<fd, std::function<void(int)>>`（read_callbacks_ / write_callbacks_）——"以 fd 为 key 的简化 Channel"。注意：CLAUDE.md 描述的是 Channel 抽象，**实现与文档不完全一致**
- **wakeup_fd**（eventfd, NONBLOCK|CLOEXEC）：跨线程唤醒，事件单独处理（不在两个回调 map 中）
- **超时驱动**：`TimerWheel`（60 槽、1s tick、`kMaxCycles=10`，O(1) 增删）挂载在 EventLoop 上，Loop 顶部轮询驱动——精度 1s
- **BatchIOHandler**：`GroupEvents` 按 error > read > write 优先级分组（同 fd 多类型去重、error 优先级剔除），然后**分组顺序 for 循环调用**（实现注释自述"can be optimized with prefetching or SIMD"——当前为分组顺序调用，非真正的预取/批处理优化）
- `ProcessEvents` 对 `EPOLLERR/EPOLLHUP` 走 error 回调：`RemoveEvent(fd)`（错误回调里只移除事件，不主动关闭连接——关闭由 Connection 层感知）

---

## 6. 连接生命周期（Connection: 624 行）

### 6.1 状态机（7 态定义，业务路径半启用）

```text
kConnecting → kConnected → kReading → kProcessing → kWriting → kConnected(复用)
                ↓ 任何状态 → kClosing → kClosed（终止态，不可逆）
```

- `CanTransition` 校验：同状态允许；kClosed 不可迁出；任意 → kClosing/kClosed 允许；正常链：Connecting→Connected→Reading→Processing→Writing→Connected
- 非法转移 → 强制 `kClosing` + 异步 `HandleClose`
- **实际调用点（grep `Transition(` 全量）**：仅 `ConnectEstablished`(→kConnected)、`Shutdown`(→kClosing)、`HandleClose`(→kClosed)、`CloseInLoop`(→kClosing)
- **结论**：kReading/kProcessing/kWriting 无任何转移调用点 → 状态机"定义完整、业务驱动未接"（与 implementation_plan 的"状态机不完整"历史评估一致；v7.1 完成了枚举与转移表，业务路径未驱动）。**待审疑问 Q2**

### 6.2 读路径

`HandleRead`：循环 `read(fd, 4KB)` 直到 EAGAIN/EOF（ET 纪律）→ 每段更新活动时间戳（ResetIdleTimeout）→ n==0 则 `HandleClose(Success)` → 结束后背压检查（超限 `PauseReading` = 移除 EPOLLIN，仅留 EPOLLOUT）→ `message_callback_(shared_from_this(), input_buffer_)`

### 6.3 写路径

`Send`（线程安全包装）→ `SendInLoop`（`new_size > max_output_buffer` 则 `HandleClose(kTimeout)`）→ `HandleWrite`：
- `BufferChain::GetIov(iov, 16)` 拿 ≤16 个 iovec → `writev` 聚集写（零拷贝：内存块+mmap 文件块混合）
- `Advance(n)` 断点续传；发空 → `UpdateEvent(fd, EPOLLIN|EPOLLET)`；若 `kClosing` → `ShutdownInLoop()`（`shutdown(SHUT_WR)` 半关）
- 发不完（EAGAIN）→ `UpdateEvent(fd, EPOLLIN|EPOLLOUT|EPOLLET)`
- （资源版 `Send(std::shared_ptr<StaticResource>)` 同理，用于静态文件零拷贝）

### 6.4 关闭路径（v7.2 统一入口）

`Close(reason)`（线程安全）→ `CloseInLoop`：
- 已 closing/closed → 忽略（幂等）
- 系统错误/超时 → 立即 `HandleClose`
- 资源限制（kBufferFull/kMemoryLimit）→ `Transition(kClosing)` + `ShutdownInLoop`（优雅：等 buffer 排空再 SHUT_WR）
- 其他 → 优雅关闭

`HandleClose`：`Transition(kClosed)` → `RemoveEvent` → `keep_alive_manager_->OnConnectionClose` → `close_callback_`（`Server::RemoveConnection` 擦除 map + 插件 NotifyConnectionClose）。析构 `::close(fd)`。

### 6.5 超时管理（三类，全部走事件循环内 TimerWheel）

| 类型 | 生效状态 | 触发动作 |
| :--- | :--- | :--- |
| 读超时 | kReading | `HandleClose(kTimeout)` |
| 写超时 | kWriting | `HandleClose(kTimeout)` |
| 空闲超时 | kConnected | `HandleClose(kTimeout)` |

配置默认：`connection_timeout`（读/写共用）、`keep_alive_timeout`（空闲，默认 15s）。`UpdateActivityTimestamp` 每次 IO 活动重置空闲超时。

---

## 7. HTTP 链路（main.cpp 的 SetOnMessage 回调）

```text
conn 数据到达 → HandleRead 攒入 input_buffer_ → message_callback_（IO 线程内同步执行）：
  while (buffer 存在 "\r\n\r\n") {
    parser->Parse(buffer)              // HttpRequest：请求行 + headers（键转小写）
      → conn->OnRequestStart(IsKeepAlive, keep_alive_timeout)   // KeepAlive 状态入表
      → plugin.NotifyRequestStart(*parser)
      → RequestValidator.ValidateRequest(*parser)               // 路径遍历/大小/方法校验
      → HttpResponse.Init + MakeResponse                        // 静态资源走 mmap 零拷贝
      → plugin.NotifyRequestComplete(*parser, response)
      → conn->Send(response header) / Send(file body | body)
      → buffer.erase(0, header_end + 4)     // 只消费 header（含 \r\n\r\n）
      → parser->Reset() → conn->OnRequestComplete()
      → status >= 400 → conn->Shutdown() / 解析失败 → buffer.clear() + Shutdown()
  }
```

### 7.1 解析器双轨（重要发现）

| 解析器 | 命名空间/接口 | 状态 |
| :--- | :--- | :--- |
| `HttpRequest` | `tinywebserver::`，`Parse(std::string&)` | **主线**（main.cpp/connection 使用） |
| `HttpParser` | 全局，状态机版（kRequestLine/kHeaders/kDone/kError），`Parse(const std::string&)` | **零引用**（grep 全库仅自身定义/实现各 1 处）——遗留/预留死代码 |

### 7.2 body 处理缺口

- `Parse` 只解析 header；`buffer.erase(0, header_end+4)` 只删到 `\r\n\r\n` 结束
- **POST body 若随包到达会残留在 input_buffer_**，等待下一次 read 事件或超时（注释："当前简单模型下…Body 逻辑需视业务而定"）
- `GetContentLength()` 已实现（stoll 解析 content-length），但 main.cpp 未调用
- `RequestValidator` 返回的 WebError → 状态码映射：kInvalidPath→403、kRequestTooLarge→413、kUnsupportedMethod→405、其余→400

---

## 8. 基础设施盘点

| 模块 | 实现 | 备注 |
| :--- | :--- | :--- |
| 日志 | `Logger`（宏 LOG_INFO 等，单例）+ `AsyncLogger`（双缓冲 4MB，最多 16 缓冲批量写盘，flush 3s）+ `StructuredLogger`（配置化；json.hpp 依赖） | 三套并存；main 同时 Init 前两者。待审 Q8 |
| 配置 | `ServerConfig` + `json_simple`（server/limits/logging/static 四节） | 有默认值兜底；`--config` 指定 |
| 定时器 | `TimerWheel`（时间轮，**主线**，EventLoop 内置）+ `TimerManager`（最小堆 + fd→index 映射，独立类） | 两套并存；TimerManager 未见主线调用。待审 Q6 |
| 线程池 | `EventLoopThreadPool`（I/O 线程）+ `ThreadPool`（通用任务池：queue+mutex+cv 生产者-消费者） | 后者主线未见调用点。待审 Q5 |
| 资源边界 | `ConnectionLimits`（默认 64KB 输入/1MB 输出）+ `config limits` 覆盖 | 背压 = PauseReading（移除 EPOLLIN） |
| 安全 | `RequestValidator`（路径规范化/目录遍历防护）+ 连接数上限（connection_limits.h） | |
| 指标/插件 | `ServerMetrics`（bytes 发送统计等）+ `PluginManager`（连接开/关、请求开始/完成 4 类生命周期事件，模板注册） | 扩展点 |
| 零拷贝 | `StaticResource`（mmap 文件）+ `BufferChain` + `writev` 聚集写 + 断点续传 | |
| HTTP/2 | `include/http2/`：h2_connection / h2_frame_parser / h2_stream / hpack_decoder | 未出现在主线 include 链。待审 Q4 |

---

## 9. 待审疑问清单（结对审核接口）

### 最需确认（4 条）

- **Q1** `HttpParser`（状态机版）全库零引用——遗留死代码（忽略/后续移除），还是计划接回主线？
- **Q2** 状态机 `kReading/kProcessing/kWriting` 无转移调用点——业务路径确实未驱动（状态机半启用），还是存在非 `Transition(` 的驱动方式？（我 grep 的是 `Transition(`
- **Q3** HTTP body 残留 input_buffer_——POST 有意不支持（当前为 GET 静态服务场景），还是半成品待接（需要改 `buffer.erase` 按 Content-Length 消费）？
- **Q4** `include/http2/` 四文件——v7.5+ 已接入主线（我未找到引用），还是预留骨架？

### 次要（4 条）

- **Q5** `ThreadPool`（通用任务池）主线调用点：未找到——未来"业务处理线程池"的预留？
- **Q6** `TimerManager`（最小堆）与 `TimerWheel`（时间轮）并存：前者是否已废弃/旧版，还是特定场景使用？
- **Q7** `EventLoop::AddTimer` 跨线程分支 `RunInLoop([...,&result])` 捕获局部引用——若未来有调用方在非 loop 线程直调会悬垂引用；当前安全（`Connection::SetupTimeout` 先行线程检查）。是否值得加固？
- **Q8** 日志三套并存（Logger 宏层 / AsyncLogger 双缓冲层 / StructuredLogger 结构层）——刻意分层，还是历史叠加？

---

## 10. 关键设计决策提炼（Why/How）

1. **无 Channel 类，用 fd→回调 map**：简单直接；代价是回调生命周期需与 fd 一致（弱引用 + RemoveConnection 兜底）
2. **业务处理在 IO 线程同步**：无任务队列、无数据复制；代价是大文件/慢处理会阻塞该 Sub Reactor（当前静态资源 mmap 缓解）
3. **ET + 用户态 drain**：read/accept 循环到 EAGAIN 是纪律性要求，遗漏即丢事件
4. **时间轮挂 loop 轮询**（无独立定时器线程）：O(1)、无跨线程同步；代价 1s 精度 + loop 每 1s 内必醒（next_timeout ≤1000 保证）
5. **背压 = 暂停读**（移除 EPOLLIN）而非丢弃：逻辑正确；代价是 TCP 窗口生效前输入缓冲可涨到限制值
6. **KeepAlive 独立管理器**：连接状态机之外单独维护 fd→keep-alive 状态表（OnRequestStart/Complete 通知），职责分离

---

*文档基于 2026-09-03 通读 src/main.cpp、src/server.cpp、src/connection.cpp、src/http_request.cpp、reactor/event_loop.cpp、reactor/batch_io_handler.cpp、reactor/event_loop_thread_pool.cpp 及各 include 头文件整理。*
