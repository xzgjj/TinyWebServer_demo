//

#ifndef CONNECTION_H
#define CONNECTION_H

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <atomic>

#include "http_request.h"
#include "buffer_chain.h" 
#include "static_resource_manager.h" // for StaticResource


class HttpRequest; 
class EventLoop;
class StaticResource; // 确保 StaticResource 也能被识别

enum class ConnState { kConnecting, kConnected, kDisconnecting, kDisconnected };

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using MessageCallback = std::function<void(std::shared_ptr<Connection>, const std::string&)>;
    using CloseCallback = std::function<void(int)>;

    Connection(int fd, EventLoop* loop);
    ~Connection();

    // 初始化连接，必须在 loop 线程调用
    void ConnectEstablished();
    
    // 发送数据接口 (线程安全)
    void Send(const std::string& data);
    void Send(const char* data, size_t len);
    // 【新增】零拷贝发送静态资源
    void Send(std::shared_ptr<StaticResource> resource);

    // 关闭连接 (线程安全)
    void Shutdown();

    // 状态与属性
    int GetFd() const { return fd_; }
    EventLoop* GetLoop() const { return loop_; }
    bool IsConnected() const { return state_ == ConnState::kConnected; }

    void SetMessageCallback(MessageCallback cb) { message_callback_ = std::move(cb); }
    void SetCloseCallback(CloseCallback cb) { close_callback_ = std::move(cb); }

    // HTTP 相关辅助
    std::string& GetInputBuffer() { return input_buffer_; }
    std::shared_ptr<HttpRequest> GetHttpParser() { return http_parser_; }
    
    // 【新增】清空读缓冲区 (用于长连接复用)
    void ClearReadBuffer() { input_buffer_.clear(); }

private:
    void HandleRead(int fd);
    void HandleWrite(int fd);
    void HandleClose(int fd);
    void HandleError(int fd);
    
    void SendInLoop(const std::string& data);
    void SendResourceInLoop(std::shared_ptr<StaticResource> res);
    void ShutdownInLoop();

    EventLoop* loop_;
    int fd_;
    std::atomic<ConnState> state_;

    // 读缓冲区：依然保持 string，处理 HTTP 文本协议头
    std::string input_buffer_;
    
    // 【修改】写缓冲区：升级为支持 Scatter/Gather 的链式缓冲
    BufferChain output_buffer_;

    std::shared_ptr<HttpRequest> http_parser_;
    MessageCallback message_callback_;
    CloseCallback close_callback_;
};

#endif