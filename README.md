
# TinyWebServer v5 – Multi-Reactor 高性能 C++ Web Server

> **定位说明**：
> 本项目是一个以 **Reactor 模式（epoll）** 为核心、面向工程实践与面试深度的 C++ 高性能 Web Server 示例实现。当前版本（v5）重点在 **多 Reactor 架构、事件驱动模型、连接生命周期管理与工程化代码结构**，并为后续 v6「工程级 Reactor」演进预留设计空间。

---

## 一、项目目标与边界

### 1. 项目目标

* 构建一个**结构清晰、职责明确**的 Reactor 架构示例
* 从**工程视角**理解 epoll 驱动的事件循环
* 覆盖 **网络 IO、并发模型、状态机、资源管理** 等核心问题
* 具备一定深度（Why / How / Trade-off）

### 2. 非目标

现阶段demo版本

* ❌ 不追求极限性能 Benchmark
* ❌ 不实现完整 HTTP/1.1 RFC
* ❌ 不实现完整生产级日志 / TLS / HTTP2

> 本项目是**工程能力放大器**，不是“功能堆砌型 Demo”。

---

## 二、整体架构概览（v5）

```
        +------------------+
        |    Acceptor      |
        |  (Main Reactor)  |
        +--------+---------+
                 |
          新连接分发
                 v
        +------------------+     +------------------+
        | Sub Reactor #1   | ... | Sub Reactor #N   |
        | epoll + loop     |     | epoll + loop     |
        +--------+---------+     +--------+---------+
                 |                        |
            Connection                Connection
```

### 架构角色说明

| 组件       | 职责                             |
| ---------- | -------------------------------- |
| Acceptor   | 监听端口、accept 新连接、分发 fd |
| EventLoop  | 封装 epoll_wait + 回调派发       |
| Channel    | fd 与事件的抽象（事件 → 回调）   |
| Connection | 一个 TCP 连接的生命周期与状态机  |
| ThreadPool | Sub Reactor 线程管理             |

> **核心思想**：
>
> * epoll 只做一件事：**告诉你“ fd 发生了什么”**
> * Reactor 做一件事：**事件 → 回调 → 状态推进**

---

## 三、Reactor 模式的工程理解

### 1. 从 epoll 到 Reactor 的抽象跃迁

#### epoll 的本质

* 内核维护一个 **就绪事件队列**
* 用户态通过 `epoll_wait` 获取事件集合

#### Reactor 的价值

* 把：

  * fd
  * 感兴趣的事件
  * 事件触发后的处理逻辑

  封装为 **Channel**

```
fd + events + callback  => Channel
```

> **Reactor = 事件分发器，而不是业务执行者**

---

### 2. Connection 是“状态机”，不是 socket 封装

一个成熟的 Reactor 项目，**真正的复杂度在 Connection**。

#### Connection 的核心状态

* Connecting
* Connected
* Reading
* Writing
* Closing
* Closed

#### 状态推进驱动组成

* epoll 事件（EPOLLIN / EPOLLOUT / ERR / HUP）
* 业务处理结果（是否还有待发送数据）

> 面试关键点：
>
> * Reactor 不“读写数据”，它**驱动状态变化**

---

### 3.  Multi-Reactor 的优势

#### 单 Reactor 的瓶颈

* epoll_wait 是串行的
* 回调执行时间不可控

#### Multi-Reactor 的拆解思路

| 层级         | 关注点             |
| ------------ | ------------------ |
| Main Reactor | 连接建立（accept） |
| Sub Reactor  | IO 事件驱动        |

#### 关键设计决策

* **一个 fd 只属于一个 EventLoop**
* 连接建立后不跨线程迁移 fd

---

## 四、关键实现的版本情况（结合 v5 现状）

### 1. 异步日志实现情况：

* ⚠️ **部分具备接口结构，但非完整工程级实现**
* 当前日志更接近：

  * 简化版异步写
  * 缺少：

    * 明确的 back-pressure
    * 有界队列
    * flush 策略与丢弃策略

> 结论：**不是严格意义上的工程级异步日志**

---

### 2. mmap 零拷贝实现情况：

* ✔ 使用了 mmap/sendfile 等零拷贝思想
* ⚠ 但：

  * 未建立完整生命周期封装
  * 未限制单连接映射大小
  * 未统一异常回收路径

> 属于：**“机制正确，工程防护不足”**

---

### 3. 内存与资源管理

#### 优点

* RAII 思维较明确
* fd / epoll 资源基本成对释放

#### 风险点

* 大响应未设置内存上限
* 未引入 LRU / Cache 淘汰策略
* 多线程下 Connection 生命周期需进一步收紧

---

## 五、问题清单

### Reactor / epoll

1. 为什么 epoll 适合高并发而不是高吞吐？
   高并发：指 大量连接同时存在，但每个连接可能很少活跃
   epoll 采用 事件驱动 + 内核监听，在空闲连接多、活跃连接少时 CPU 不会空转
   高吞吐：指 连接活跃且数据量大，这更多依赖 IO 带宽和应用处理能力，epoll 只是事件通知，吞吐不一定比 select/poll 高
   结论：epoll 擅长处理 大数量空闲/低活跃连接 → 高并发

---

2. LT 与 ET 在 Reactor 设计中的取舍？
   模式	特点	Reactor 中取舍
   LT（Level Triggered）	fd 可读/可写 → epoll_wait 持续返回	简单，代码容易写，不容易漏事件，但 重复触发，可能多余 CPU
   ET（Edge Triggered）	只在状态改变时通知	高效，减少 epoll_wait 调用 → CPU 利好，但要求 一次性读写直到 EAGAIN，否则可能漏事件

工程取舍：
多数 Reactor accept + read/write 使用 ET 提升性能
对简单/低并发 fd 可以 LT，容错性高

---

3. 一个 fd 能否同时被两个 epoll 监听？为什么？
   可以，但不推荐内核允许多个 epoll 实例注册同一个 fd

问题：
事件通知不可控：同一事件可能通知两个 epoll，造成 重复触发或惊群
复杂资源管理，难保证线程安全
工程实践：一个 fd 对应一个 EventLoop/epoll，简化状态管理

---

### Connection / 状态机

4. Connection 为什么不能只用一个 read/write 回调？
   一个 fd 的状态可能很复杂：读缓冲可能已满  写缓冲可能还有未发送数据
   单回调不能区分不同状态，可能：写事件被触发但写缓冲为空 → 冗余 CPU 读事件被触发但 Connection 已关闭 → 错误
   状态机设计：每个阶段（read_header/read_body/write_response）独立处理 → 更清晰可控

---


6. 半关闭（FIN）如何在 Reactor 中处理？
   内核 fd 收到 FIN → read 返回 0
   Reactor 处理方式：读端关闭：read 返回 0 → 标记 Connection 关闭读方向
   写端可能还没关闭：继续发送剩余响应
   状态机必须区分 半关闭 与 完全关闭，防止数据丢失

---

7. 写事件什么时候应该关闭监听？
   写事件的 fd 监听可以关闭的条件：

写缓冲 全部发送完
Connection 不再接受新的数据
否则继续监听 防止缓冲区可写时未写完
注意：过早关闭写监听 → 数据丢失

---

### 并发与内核

7. accept 是否应该在多线程中做？
   多线程 accept 存在惊群（thundering herd）问题
   工程实践：
   一个 EventLoop/线程 负责 accept → 分发到 Worker
   或者使用 SO_REUSEPORT + 多线程 accept
   简单、高性能方案：主线程 accept → 分发到 I/O 线程池


8. epoll_wait 是否会产生惊群？
   单 epoll_wait 不会
   多线程共享同一 epoll 实例时可能惊群
   多线程被唤醒，但只有一个线程能成功 accept/read
   解决方案：
   一个线程对应一个 epoll（主流 Reactor 模式）
   或使用 ET + eventfd + queue 分发事件

---

10. 为什么“一个 EventLoop 一个线程”是主流？
    原因：
    避免多线程共享 epoll → 惊群、锁竞争
    每个 EventLoop 独立管理 Connection → 状态机单线程安全
    简化调度逻辑 → 高可维护性、高性能

扩展：
多核机器：多个 EventLoop 线程 + 任务分发 → 高并发处理
保证单线程 Reactor 逻辑简单、避免复杂锁


---

## 六、v6：Reactor 演进方向

### 高优先级

* 明确 Connection 状态机（enum + 状态转移表）
* 有界异步日志队列（丢弃/阻塞策略）
* 统一错误码与关闭路径

### 中优先级

* 读写缓冲区水位控制
* mmap 文件缓存 + LRU 淘汰
* TimerWheel / TimeHeap 管理超时连接

### 低优先级

* 更细粒度锁优化
* 编译期优化与 inline 策略

---

## 七、测试、部署与工程化建议

### 测试

* Connection 状态机可单测
* EventLoop 可 Mock epoll

### 运维

* 日志等级与输出路径外部化
* 暴露连接数、活跃 fd、事件循环延迟指标

### 技术债务提示

* 当前版本demo
* 距离生产仍需：

  * 更严格的边界控制
  * 更系统的压测与故障演练

### 调试与运行

bash
mkdir -p build
cd build
cmake ..
make

make -j4


python3 tools.py clean

mkdir -p build
cd build

配置 CMake
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -O0 -g" \
      ..


cmake --build . --parallel


cmake -DCMAKE_BUILD_TYPE=Debug ..
在一个终端（左侧）启动服务器的 GDB 调试。 
gdb ./server 


第一步：清理环境   
python3 tools.py clean
第二步：全量编译并运行所有测试
python3 tools.py all
python3 tools.py debug --target test_backpressure

python3 tools.py build
python3 tools.py test

使用 GDB 调试特定测试
python3 tools.py debug --target test_backpressure

find . -type f -exec touch {} +
终端 1 (启动服务器):  ./server
终端 2 (运行测试脚本):

sudo kill -9 $(sudo lsof -t -i:8080)



---

## 八、总结

> **项目目的** 

>* 理解 IO 事件的生命周期
>* 能把内核机制转化为工程抽象
>* 知道“什么该做，什么不该现在做”






