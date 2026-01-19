//

#ifndef CONNECTION_H
#define CONNECTION_H

#include <memory>
#include <string>
#include <vector>
#include "http_request.h"
#include <functional>

class EventLoop;

enum class ConnState { kConnecting, kConnected, kDisconnecting, kDisconnected };

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using MessageCallback = std::function<void(std::shared_ptr<Connection>, const std::string&)>;
    using CloseCallback = std::function<void(int)>;

    Connection(int fd, EventLoop* loop);
    ~Connection();

    // 【新增】初始化连接，必须在 loop 线程调用
    void ConnectEstablished();
    void ClearReadBuffer() { input_buffer_.clear(); }
    
    // 【修改】发送数据，线程安全
    void Send(const std::string& data);
    void Send(const char* data, size_t len);

    // 【新增】关闭连接，线程安全
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

private:
    // 只能在 loop_ 线程中调用的私有方法
    void HandleRead(int fd);
    void HandleWrite(int fd);
    void HandleClose(int fd);
    void HandleError(int fd);
    
    void SendInLoop(const std::string& data);
    void ShutdownInLoop();

    EventLoop* loop_;
    int fd_;
    ConnState state_;

    // 缓冲区（不再需要互斥锁，因为只在 IO 线程操作）
    std::string input_buffer_;
    std::string output_buffer_;

    std::shared_ptr<HttpRequest> http_parser_;
    MessageCallback message_callback_;
    CloseCallback close_callback_;
};

#endif