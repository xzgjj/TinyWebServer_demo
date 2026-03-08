
//

// 添加缺失的头文件
#include "server.h"
#include "connection.h"
#include "Logger.h"
#include "config/server_config.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <functional>

// ... 原有代码保持不变 ...
int CreateListenSocket(unsigned short port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port); // 【解决 unused parameter】
    addr.sin_addr.s_addr = INADDR_ANY;

    // 设置 SO_REUSEADDR 方便调试
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}


Server::Server(const std::string& ip, int port,
               const SOReusePortOptions& reuseport_opts)
    : main_loop_(std::make_unique<EventLoop>()),
      thread_pool_(std::make_unique<EventLoopThreadPool>(main_loop_.get())),
      listen_fd_(-1),
      ip_(ip),
      port_(port),
      backlog_(1024),
      config_(nullptr),
      keep_alive_manager_(std::make_unique<tinywebserver::KeepAliveManager>()),
      reuseport_opts_(reuseport_opts),
      multi_listen_socket_(nullptr) {

    // 根据配置选择启动模式
    if (reuseport_opts_.enabled) {
        SetupSOReusePortMode();
    } else {
        SetupTraditionalMode();
    }

    LOG_INFO("Server started on %s:%d (mode: %s)",
             ip.c_str(), port,
             reuseport_opts_.enabled ? "SO_REUSEPORT" : "traditional");
}

Server::Server(const std::shared_ptr<tinywebserver::ServerConfig>& config)
    : config_(config),
      keep_alive_manager_(std::make_unique<tinywebserver::KeepAliveManager>(
          std::chrono::seconds(config->GetLimitsOptions().keep_alive_timeout))) {

    if (!config) {
        LOG_ERROR("Invalid configuration provided");
        throw std::invalid_argument("ServerConfig cannot be null");
    }

    auto server_opts = config->GetServerOptions();
    ip_ = server_opts.ip;
    port_ = server_opts.port;
    backlog_ = server_opts.backlog;

    // 从配置中提取 SO_REUSEPORT 选项
    reuseport_opts_.enabled = server_opts.use_so_reuseport;
    reuseport_opts_.num_listen_sockets = server_opts.so_reuseport_sockets;
    if (reuseport_opts_.num_listen_sockets == 0) {
        reuseport_opts_.num_listen_sockets = server_opts.threads;
    }

    main_loop_ = std::make_unique<EventLoop>();
    thread_pool_ = std::make_unique<EventLoopThreadPool>(main_loop_.get());
    // 设置线程池线程数从配置
    thread_pool_->SetThreadNum(server_opts.threads);

    // 根据配置选择启动模式
    if (reuseport_opts_.enabled) {
        SetupSOReusePortMode();
    } else {
        SetupTraditionalMode();
    }

    LOG_INFO("Server started on %s:%d (config, mode: %s)",
             server_opts.ip.c_str(), server_opts.port,
             reuseport_opts_.enabled ? "SO_REUSEPORT" : "traditional");
}

Server::~Server() {
    Stop();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
    }
    // MultiListenSocket 会自动关闭所有socket
}



void Server::Start() {
    // 1. 启动线程池 (SubLoops)
    // 线程数已在构造函数中设置，此处只需启动
    thread_pool_->Start();

    // 2. 启动主循环 (MainLoop)

}

void Server::Stop() {
    main_loop_->Stop();
    thread_pool_->Stop(); // 需在 ThreadPool 中实现 Stop
}

void Server::Run() {
    LOG_INFO("Server::Run() 开始事件循环");

    // 在传统模式下，注册监听事件到主 Reactor
    // 在 SO_REUSEPORT 模式下，监听事件已经在各个 Sub Reactor 中注册
    if (!reuseport_opts_.enabled && listen_fd_ >= 0) {
        LOG_INFO("Server::Run() 注册监听事件到主 EventLoop, fd=%d", listen_fd_);
        main_loop_->UpdateEvent(listen_fd_, EPOLLIN | EPOLLET);  // 边缘触发模式
        LOG_INFO("Server::Run() 监听事件注册完成");
    } else if (reuseport_opts_.enabled) {
        LOG_INFO("Server::Run() SO_REUSEPORT 模式：监听事件已在各个 Sub Reactor 中注册");
    }

    main_loop_->Loop();
    LOG_INFO("Server::Run() 事件循环结束");
}

void Server::HandleAccept(int listen_fd) {
    LOG_INFO("Server::HandleAccept: 新的accept事件，listen_fd=%d", listen_fd);
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // 循环 accept 处理并发连接（ET模式下必须读完）
    while (true) {
        int conn_fd = accept4(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len, 
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (conn_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 处理完毕
            } else if (errno == EINTR) {
                continue;
            } else {
                LOG_ERROR("Accept error");
                break;
            }
        }
        
        // 【核心重构 V5】：获取一个 SubLoop，而不是使用当前的 MainLoop
        EventLoop* io_loop = thread_pool_->GetNextLoop();
        
        // 创建连接对象
        // 注意：连接归属于 io_loop，但目前我们在 MainLoop 线程中
        auto conn = std::make_shared<Connection>(conn_fd, io_loop, config_, keep_alive_manager_.get());
        conn->SetMessageCallback(on_message_);
        conn->SetCloseCallback(std::bind(&Server::RemoveConnection, this, std::placeholders::_1));

        {
            // 加锁保护 connections_ 映射表（因为 RemoveConnection 可能在其他线程触发）
            std::lock_guard<std::mutex> lock(conn_mutex_);
            connections_[conn_fd] = conn;
        }

        LOG_INFO("New connection fd=%d assigned to loop thread %s", 
                 conn_fd, io_loop->GetThreadIdString().c_str());

        // 【关键】：将“连接建立”的任务派发到 io_loop 线程执行
        // 这样保证 Connection 的 Channel 操作完全在 IO 线程内
        io_loop->RunInLoop([conn]() {
            conn->ConnectEstablished();
        });
    }
}

void Server::RemoveConnection(int fd) {
    // 注意：此函数可能被 SubLoop 线程调用
    std::lock_guard<std::mutex> lock(conn_mutex_);
    size_t n = connections_.erase(fd);
    if (n == 1) {
        LOG_INFO("Connection fd=%d removed", fd);
    }
}

void Server::SetupSOReusePortMode() {
    // 创建多监听socket
    size_t num_sockets = reuseport_opts_.num_listen_sockets;
    if (num_sockets == 0) {
        num_sockets = thread_pool_->GetThreadCount();
    }

    multi_listen_socket_ = std::make_unique<MultiListenSocket>(
        ip_, port_, num_sockets, backlog_);

    if (!multi_listen_socket_->IsValid()) {
        throw std::runtime_error("Failed to create SO_REUSEPORT sockets: " +
                                 multi_listen_socket_->GetError());
    }

    // 启动线程池
    thread_pool_->Start();

    // 分配监听socket给每个线程
    thread_pool_->AssignListenSockets(multi_listen_socket_->GetAllSocketFds());

    // 为每个 Sub Reactor 设置自己的 accept 回调
    size_t num_threads = thread_pool_->GetThreadCount();
    const auto& all_fds = multi_listen_socket_->GetAllSocketFds();
    size_t num_sockets_actual = all_fds.size();

    for (size_t i = 0; i < num_threads; ++i) {
        EventLoop* sub_loop = thread_pool_->GetLoopByIndex(i);
        if (!sub_loop) {
            LOG_ERROR("Failed to get EventLoop for thread %zu", i);
            continue;
        }

        // 计算分配给该线程的socket索引
        size_t socket_idx = i % num_sockets_actual;
        int listen_fd = all_fds[socket_idx];

        // 在 Sub Reactor 线程中设置回调
        sub_loop->RunInLoop([this, sub_loop, listen_fd]() {
            sub_loop->SetReadCallback(listen_fd,
                [this, listen_fd, sub_loop]([[maybe_unused]] int fd) {
                    this->HandleAcceptInSubReactor(listen_fd, sub_loop);
                });
            sub_loop->UpdateEvent(listen_fd, EPOLLIN | EPOLLET);
        });

        LOG_DEBUG("Assigned socket fd=%d to Sub Reactor %zu", listen_fd, i);
    }

    // 主 Reactor 不监听任何socket（或可以监听一个用于管理）
    // 这里我们选择不监听，让所有accept都在Sub Reactor中进行

    LOG_INFO("SO_REUSEPORT mode enabled with %zu listening sockets", num_sockets);
}

void Server::SetupTraditionalMode() {
    // 创建单个监听socket
    listen_fd_ = CreateListenSocket(static_cast<unsigned short>(port_), backlog_);
    if (listen_fd_ < 0) {
        LOG_ERROR("Failed to create listen socket on port %d", port_);
        throw std::runtime_error("Failed to create listen socket");
    }

    // 设置监听socket的读事件回调（当有新连接时触发）
    main_loop_->SetReadCallback(listen_fd_,
        [this, listen_fd = listen_fd_]([[maybe_unused]] int fd) {
            this->HandleAccept(listen_fd);
        });

    LOG_INFO("Traditional mode enabled with single listening socket");
}

void Server::HandleAcceptInSubReactor(int listen_fd, EventLoop* sub_loop) {
    LOG_INFO("Server::HandleAcceptInSubReactor: new accept event, listen_fd=%d", listen_fd);
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // 循环 accept 处理并发连接（ET模式下必须读完）
    while (true) {
        int conn_fd = accept4(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (conn_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 处理完毕
            } else if (errno == EINTR) {
                continue;
            } else {
                LOG_ERROR("Accept error on listen_fd=%d", listen_fd);
                break;
            }
        }

        // 在当前 Sub Reactor 中创建连接（无需跨线程分配）
        auto conn = std::make_shared<Connection>(conn_fd, sub_loop, config_, keep_alive_manager_.get());
        conn->SetMessageCallback(on_message_);
        conn->SetCloseCallback(std::bind(&Server::RemoveConnection, this, std::placeholders::_1));

        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            connections_[conn_fd] = conn;
        }

        LOG_INFO("New connection fd=%d accepted in Sub Reactor %s",
                 conn_fd, sub_loop->GetThreadIdString().c_str());

        // 在当前 Sub Reactor 线程中建立连接
        sub_loop->RunInLoop([conn]() {
            conn->ConnectEstablished();
        });
    }
}

