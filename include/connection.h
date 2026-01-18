//

#ifndef CONNECTION_H
#define CONNECTION_H

#include <unistd.h>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <mutex>

enum class ConnState { OPEN, CLOSING, CLOSED };

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using MessageCallback = std::function<void(std::shared_ptr<Connection>, const std::string&)>;

    explicit Connection(int fd);
    ~Connection();

    // 关键：去掉这里的 { return ... }，只留声明
    int Fd() const noexcept;
    ConnState State() const noexcept;
    bool HasPendingWrite() const;

    void HandleRead(const MessageCallback& cb);
    void HandleWrite();
    void Send(const std::string& data);
    bool TryFlushWriteBuffer();
    void Close();

private:
    int fd_;
    ConnState state_;
    std::vector<char> read_buffer_;
    std::vector<char> write_buffer_;
    std::mutex buffer_mutex_; // 确保这里有锁
};

#endif