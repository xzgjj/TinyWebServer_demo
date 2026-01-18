//

#ifndef CONNECTION_H
#define CONNECTION_H

#include <memory>
#include <string>
#include <mutex>
#include <functional>
#include "http_request.h"

enum class ConnState { OPEN, CLOSED };

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using MessageCallback = std::function<void(std::shared_ptr<Connection>, const std::string&)>;

    explicit Connection(int fd);
    ~Connection();

    // 简单的getter函数直接在类内定义（推荐做法）
    int Fd() const { return fd_; }
    ConnState State() const { return state_; }

    std::shared_ptr<HttpRequest> GetHttpParser() { return http_parser_; }
    std::string& GetInputBuffer() { return input_buffer_; }

    void HandleRead(const MessageCallback& cb);
    void HandleWrite();
    void Send(const std::string& data);
    void Close();

    bool HasPendingWrite() const;
    bool TryFlushWriteBuffer();

    bool IsWriteBlocked() const { return write_blocked_; }
    void SetWriteBlocked(bool b) { write_blocked_ = b; }

private:
    int fd_;
    ConnState state_;
    std::string input_buffer_;      // 输入缓冲区
    std::string write_buffer_;      // 输出缓冲区（与cpp中一致）
    std::mutex buffer_mutex_;       // 缓冲区互斥锁
    std::shared_ptr<HttpRequest> http_parser_;

    bool write_blocked_ = false; // 标记内核缓冲区是否已满
};

#endif // CONNECTION_H
