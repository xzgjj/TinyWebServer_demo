//

#include "connection.h"
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <cstring>
#include <iostream>

Connection::Connection(int fd)
    : fd_(fd), state_(ConnState::OPEN), http_parser_(std::make_shared<HttpRequest>()) {}

Connection::~Connection() {
    if (fd_ >= 0) {
        ::close(fd_);
        std::cout << "[Conn] Socket " << fd_ << " closed." << std::endl;
    }
}

// 删除重复定义的函数（第21-23行）
// int Connection::Fd() const noexcept { return fd_; }
// ConnState Connection::State() const noexcept { return state_; }

void Connection::HandleRead(const MessageCallback& cb) {
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            // 1. 将读取到的字节流追加到应用层缓冲区
            input_buffer_.append(buf, n);
            // 2. 调用回调，让业务层（main.cpp）去缓冲区里解析
            if (cb) cb(shared_from_this(), "");
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            state_ = ConnState::CLOSED;
            break;
        }
    }
}

bool Connection::HasPendingWrite() const {
    // 使用 write_buffer_（与头文件一致）
    return !write_buffer_.empty();
}

void Connection::HandleWrite() {
    if (state_ == ConnState::CLOSED) return;
    TryFlushWriteBuffer();
}


void Connection::Send(const std::string& data) {
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (state_ != ConnState::OPEN) return;
        write_buffer_.append(data); // 使用 append
    }
    
    // 尝试立即刷新缓冲区，发不完的部分会留在 write_buffer_ 
    // 后续由 EpollReactor 触发 EPOLLOUT 事件时调用 HandleWrite 完成发送
    TryFlushWriteBuffer();
}


bool Connection::TryFlushWriteBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (write_buffer_.empty()) {
        write_blocked_ = false;
        return true;
    }

    while (!write_buffer_.empty()) {
        ssize_t n = ::write(fd_, write_buffer_.data(), write_buffer_.size());
        if (n > 0) {
            write_buffer_.erase(0, n);
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                write_blocked_ = true; // 关键：标记被阻塞
                return false; 
            } else if (errno == EINTR) {
                continue;
            } else {
                state_ = ConnState::CLOSED;
                return false;
            }
        }
    }
    write_blocked_ = false; // 发完了
    return true;
}

void Connection::Close() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    state_ = ConnState::CLOSED;
    // 清空缓冲区
    write_buffer_.clear();
    input_buffer_.clear();
}
