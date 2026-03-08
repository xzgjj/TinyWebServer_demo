# 配置文件说明

## 配置文件结构

TinyWebServer 使用 JSON 格式的配置文件，支持以下配置节：

### 1. 服务器配置 (`server`)
- `ip`: 监听 IP 地址 (默认: "0.0.0.0")
- `port`: 监听端口 (默认: 8080)
- `threads`: Sub Reactor 线程数 (默认: CPU 核心数)
- `backlog`: listen() backlog 参数 (默认: 1024)
- `tcp_nodelay`: 启用 TCP_NODELAY (默认: true)
- `tcp_cork`: 启用 TCP_CORK (默认: false)
- `use_so_reuseport`: 启用 SO_REUSEPORT 多队列优化 (默认: false)
- `so_reuseport_sockets`: SO_REUSEPORT 监听socket数量，0表示等于线程数 (默认: 0)

### 2. 资源限制 (`limits`)
- `max_connections`: 最大并发连接数 (默认: 10000)
- `max_request_size`: 最大请求大小，单位字节 (默认: 64KB)
- `max_response_size`: 最大响应大小，单位字节 (默认: 1MB)
- `connection_timeout`: 连接超时，单位秒 (默认: 30)
- `keep_alive_timeout`: Keep-Alive 空闲超时，单位秒 (默认: 15)
- `max_input_buffer`: 单个连接输入缓冲区上限，单位字节 (默认: 64KB)
- `max_output_buffer`: 单个连接输出缓冲区上限，单位字节 (默认: 1MB)

### 3. 日志配置 (`logging`)
- `level`: 日志级别: DEBUG, INFO, WARN, ERROR, FATAL (默认: INFO)
- `file`: 日志文件路径 (默认: "logs/server.log")
- `async`: 是否启用异步日志 (默认: true)
- `queue_size`: 异步日志队列大小 (默认: 10000)
- `flush_interval`: 日志刷新间隔，单位秒 (默认: 3)

### 4. 静态资源 (`static`)
- `root`: 静态资源根目录 (默认: "./public")
- `cache_size`: 文件缓存条目数 (默认: 100)
- `cache_ttl`: 缓存生存时间，单位秒 (默认: 300)

### 5. 监控指标 (`metrics`) [可选]
- `enable_prometheus`: 是否启用 Prometheus 指标导出 (默认: true)
- `prometheus_port`: Prometheus 指标导出端口 (默认: 9090)
- `collect_interval`: 指标收集间隔，单位秒 (默认: 5)

## 使用方法

### 启动服务器时指定配置文件
```bash
./build/server --config configs/server.json
```

### 热重载配置
向服务器进程发送 SIGHUP 信号：
```bash
kill -HUP <pid>
```

### 检查当前配置
访问管理端点：
```
GET /admin/config
```

## 配置验证

配置文件加载时会进行以下验证：
1. 端口范围检查 (1-65535)
2. 线程数检查 (>=1)
3. 路径安全性检查
4. 内存限制合理性检查

如果验证失败，服务器将使用默认配置并记录警告。

## 示例

- 基础配置: `server.example.json`
- SO_REUSEPORT 优化配置: `server.so_reuseport.example.json`

## SO_REUSEPORT 优化说明

### 什么是 SO_REUSEPORT？
SO_REUSEPORT 是 Linux 3.9+ 引入的 socket 选项，允许多个 socket 绑定到相同的 IP 地址和端口。内核负责在多个监听 socket 之间负载均衡传入连接。

### 传统模式 vs SO_REUSEPORT 模式
- **传统模式**: 单个监听 socket，由 Main Reactor 接受连接后分配给 Sub Reactor
- **SO_REUSEPORT 模式**: 每个 Sub Reactor 拥有自己的监听 socket，直接接受连接

### 性能优势
1. **消除单点瓶颈**: 无 Main Reactor accept 竞争
2. **内核级负载均衡**: Linux 内核将连接分配到不同监听 socket
3. **更好的缓存局部性**: 连接直接在接受的线程中处理
4. **减少锁竞争**: 无需跨线程分配连接

### 使用建议
- **高并发场景**: 连接建立速率 > 10,000/秒时效果显著
- **Linux 3.9+**: 需要较新内核版本
- **CPU 密集型**: 当 CPU 核心数 >= 4 时推荐启用
- **监控**: 启用后监控各个线程的连接分布

### 配置示例
```json
{
  "server": {
    "use_so_reuseport": true,
    "so_reuseport_sockets": 0,  // 0 = 等于线程数
    "threads": 8
  }
}
```
