# TinyWebServer demo - Multi-Reactor High-Performance C++ Web Server

<p align="center">
  <img src="https://img.shields.io/badge/-0EA5E9?style=flat-square" alt="bar1" />
  <img src="https://img.shields.io/badge/-22C55E?style=flat-square" alt="bar2" />
  <img src="https://img.shields.io/badge/-EAB308?style=flat-square" alt="bar3" />
  <img src="https://img.shields.io/badge/-F97316?style=flat-square" alt="bar4" />
  <img src="https://img.shields.io/badge/-EF4444?style=flat-square" alt="bar5" />
</p>

> **定位说明**  
> 本项目是基于 **Reactor 模式（epoll）** 的 C++ Web Server 工程化示例。当前处于 **v7.x 演进阶段**，重点在连接治理、可观测性、安全校验与协议完整性。

---

## 一、项目目标与边界

### 1. 项目目标

- 构建结构清晰、职责明确的 Multi-Reactor 架构示例。
- 通过工程化演进，覆盖连接状态机、资源边界、错误处理、可观测性与安全控制。
- 提供可测试、可回归、可扩展的 C++ 网络服务实践样本。
- 兼顾教学价值与工程可落地性，强调 Why / How / Trade-off。

### 2. 非目标

当前版本仍是工程化示例，不追求以下目标：

- 不追求极限性能排行榜结果。
- 不承诺完整生产级特性（如 TLS 全套、完整 HTTP/2 生态、分布式控制面）。
- 不引入与主线目标无关的复杂中间件栈。

> 项目定位是“工程能力放大器”，不是“功能堆砌型 Demo”。

---

## 二、整体架构概览

当前采用 Main/Sub Reactor 分层模型：Main Reactor 专注接入，Sub Reactor 负责 I/O 事件循环与连接状态推进。

```text
        +------------------+
        |  Main Reactor    |
        | accept + dispatch|
        +--------+---------+
                 |
                 v
        +------------------+     +------------------+
        | Sub Reactor #1   | ... | Sub Reactor #N   |
        | epoll + loop     |     | epoll + loop     |
        +--------+---------+     +--------+---------+
                 |                        |
              Connection State Machine (ET + Non-blocking)
```

### 架构分层

| 层次 | 关键组件 | 职责 |
| --- | --- | --- |
| 接入层 | `Server` / Acceptor | 监听端口、accept 新连接、连接分发 |
| 事件层 | `EventLoop` / `Channel` | `epoll_wait` 事件收集与回调派发 |
| 连接层 | `Connection` | 状态机、读写缓冲、背压、超时、关闭路径 |
| 协议层 | `HttpParser` / `HttpRequest` / `HttpResponse` | 解析请求、构建响应、Keep-Alive/条件请求能力 |
| 安全层 | `RequestValidator` | 方法白名单、路径规范化、头部与请求体边界检查 |
| 配置与观测层 | `ServerConfig` / `ServerMetrics` / Structured Logger | 配置外部化、指标导出、结构化日志 |

### 核心约束

- 一个 fd 只属于一个 EventLoop，避免跨线程迁移。
- ET + 非阻塞 I/O，读写循环直到 `EAGAIN/EWOULDBLOCK`。
- 所有请求先过 `RequestValidator`，再进入业务响应路径。
- 资源必须有上限，错误路径必须可追踪、可观测。

---

## 三、Reactor 模式的工程理解

### 1. 从 epoll 到 Reactor

- `epoll` 负责事件就绪通知。
- Reactor 负责把 `fd + events + callback` 组织成可演进的执行模型。

```text
fd + events + callback => Channel
```

### 2. Connection 是状态机

连接复杂度主要来自状态推进，而不是 socket API 本身。状态迁移由两类信号驱动：

- 内核事件：`EPOLLIN / EPOLLOUT / ERR / HUP`
- 业务结果：是否继续读取、是否仍有待发送数据、是否进入关闭路径

### 3. Multi-Reactor 的价值

- 降低单循环串行瓶颈。
- 减少锁竞争与惊群影响。
- 使“一个 EventLoop 一个线程”约束更容易落地。

---

## 四、关键实现的版本情况（结合当前 v7 状态）

### 1. 版本能力矩阵

| 阶段 | 关键能力 | 状态 |
| --- | --- | --- |
| v5 基线 | Multi-Reactor、EventLoop/Channel、静态资源服务 | ✅ 已稳定 |
| v7.1 | Connection 完整状态机、读写缓冲边界、背压机制 | ✅ 已完成 |
| v7.2 | 统一错误码（`WebError`）、`Error` 封装、超时与关闭路径收敛 | ✅ 已完成 |
| v7.3 | JSON 配置系统、指标系统、结构化日志、测试分类 | ✅ 已完成 |
| v7.4 当前 | 请求安全验证、Keep-Alive、条件请求、HTTP/2 基础模块 | 🟡 已集成，持续打磨 |

### 2. 已落地的工程特性

- 连接治理：显式状态转移、超时控制、统一关闭入口。
- 安全治理：路径遍历防护、请求头/体积限制、方法白名单。
- 可观测性：结构化日志 + 运行指标。
- 配置化：静态根目录默认 `./public`，支持配置文件覆盖。

### 3. 当前风险与技术债

- HTTP/2 仍在基础能力阶段，协议完备性与兼容性测试需继续补齐。
- 性能工程（内存池、I/O 批处理、系统化 benchmark）尚未完成。
- 高压力与故障注入场景仍需进一步回归验证。

---

## 五、问题清单

### Reactor / epoll

1. 为什么 epoll 更擅长高并发而非天然高吞吐？  
   要点：epoll 优势在“只处理活跃连接”，吞吐还依赖带宽与业务处理效率。

2. ET 与 LT 如何取舍？  
   要点：ET 更高效但实现要求高；LT 更稳健但重复触发更多。

3. 一个 fd 能否被多个 epoll 监听？  
   要点：内核允许，但工程上通常禁止，避免事件重复与状态管理复杂化。

### Connection / 状态机

4. 为什么不能只用一个 read/write 回调？  
   要点：读写生命周期不同步，单回调难以表达复杂状态转移。

5. 半关闭（FIN）如何处理？  
   要点：读端关闭与写端收尾要分离，避免响应数据丢失。

6. 写事件何时取消监听？  
   要点：发送缓冲耗尽且无后续数据时取消，避免空转。

### 并发与内核

7. accept 是否应多线程并发？  
   要点：默认单 acceptor + 分发更稳；多 accept 需处理惊群与均衡策略。

8. epoll_wait 会不会惊群？  
   要点：单线程单 epoll 不会；多线程共享同一 epoll 才有风险。

9. 为什么“一个 EventLoop 一个线程”是主流？  
   要点：简化并发模型、减少锁与共享状态，工程可维护性更高。

---

## 六、Reactor 演进方向

> 

### 已落地（v7.1 ~ v7.3）

- Connection 状态机与资源边界控制。
- 统一错误处理体系与超时管理。
- 配置系统、指标系统、结构化日志。

### 当前重点（v7.4）

- 请求安全验证闭环（方法、路径、头部、请求体）。
- HTTP/1.1 完整性增强（Keep-Alive、条件请求）。
- HTTP/2 基础模块集成与测试完善。

### 下一阶段（v7.5+）

- 性能工程：分级内存池、I/O 批处理、性能回归基线。
- 扩展工程：插件边界与最小可用插件链路。
- 稳定性工程：压测、故障注入、恢复演练。

### 里程碑验收标准

- 每阶段均满足：构建通过 + 对应测试通过 + 文档同步更新。
- 新能力必须附带最小可回归用例。
- 以“可运行、可观测、可回归”为完成标准，而非功能数量。

---

## 七、测试、部署与工程化建议

### 1. 测试策略

- 单元测试：状态机、请求校验、条件请求、Keep-Alive。
- 集成测试：多连接、生命周期、异常关闭、并发行为。
- 工具链验证：ASan/UBSan、Valgrind（按环境支持情况）。

### 2. 常用命令

```bash
# 清理
python3 tools.py clean

# 构建
python3 tools.py build

# 测试
python3 tools.py test

# 一键流程
python3 tools.py all

# 调试单测
python3 tools.py debug --target test_backpressure
```

### 3. 网络行为验证（示例）

```bash
# Keep-Alive 基础验证
curl -v --keepalive-time 5 --keepalive http://localhost:8080/index.html
```

建议至少覆盖以下断言：

- Keep-Alive 多请求复用连接。
- 条件请求（`If-None-Match` / `If-Modified-Since`）行为正确。
- 非法路径与非法请求头被安全拦截。

---

## 八、总结

本项目的核心价值不在“功能数量”，而在“工程完整性”：

- 把内核事件机制转化为可维护的工程抽象。
- 把连接生命周期与资源治理做成可验证体系。
- 把配置、日志、指标与测试串成持续演进闭环。

如果你希望继续提升专业度，建议下一步补充：

1. 压测基线（QPS/延迟分位/内存占用）与版本对比表。  
2. 故障演练清单（连接风暴、慢连接、畸形请求、磁盘异常）。  
3. 关键路径时序图（accept -> parse -> validate -> respond -> close/reuse）。  

