//

//
#ifndef CONNECTION_H
#define CONNECTION_H

#include <memory>
#include <string>
#include <mutex>
#include <functional>
#include "http_request.h"

class EpollReactor; // 前向声明

enum class ConnState { OPEN, CLOSED };

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using MessageCallback = std::function<void(std::shared_ptr<Connection>, const std::string&)>;

    explicit Connection(int fd, EpollReactor* reactor);
    ~Connection();

    // 新增：供 Reactor 调用
    int Recv();                               // 从 socket 读到 input_buffer_
    int Send();                               // 将 output_buffer_ 发送到 socket
    
    // 原有的发送接口（业务层调用）
    void Send(const std::string& data);       // 将数据放入 output_buffer_ 并标记可写

    const std::string& GetReadBuffer() const { return input_buffer_; }
    void ClearReadBuffer() { input_buffer_.clear(); }

    int GetFd() const { return fd_; }
    ConnState State() const { return state_; }

    std::shared_ptr<HttpRequest> GetHttpParser() { return http_parser_; }
    std::string& GetInputBuffer() { return input_buffer_; }

    void HandleRead(const MessageCallback& cb);
    void HandleWrite();
    void Close();

    // 性能优化：检查是否真的需要更新 Epoll 状态
    bool HasPendingWrite() const { return write_offset_ < write_buffer_.size(); }
    bool NeedsEpollUpdate() const { return needs_epoll_update_; }
    void ClearUpdateFlag() { needs_epoll_update_ = false; }

    bool TryFlushWriteBuffer();

private:
    int fd_;
    EpollReactor* reactor_;
    ConnState state_;
    std::string input_buffer_;
    
    // 缓冲区优化：使用 offset 避免 O(N) erase
    std::string write_buffer_;
    size_t write_offset_ = 0; 

    bool write_blocked_ = false;
    bool needs_epoll_update_ = false; // 状态变更标记
    
    std::shared_ptr<HttpRequest> http_parser_;
    std::mutex buffer_mutex_;

    std::string output_buffer_; // 写入缓冲区（仅用于Send()函数）
};

#endif