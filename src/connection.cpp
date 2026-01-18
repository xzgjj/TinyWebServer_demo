//

#include "connection.h"
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <cstring>
#include <iostream>

Connection::Connection(int fd) : fd_(fd), state_(ConnState::OPEN) {
    read_buffer_.reserve(4096);
    write_buffer_.reserve(4096);
}

Connection::~Connection() {
    if (fd_ >= 0) {
        ::close(fd_);
        std::cout << "[Conn] Socket " << fd_ << " closed." << std::endl;
    }
}

int Connection::Fd() const noexcept { return fd_; }

ConnState Connection::State() const noexcept { return state_; }

bool Connection::HasPendingWrite() const {
    // 访问缓冲区需要加锁，防止主线程判断时子线程正在写入
    // 注意：这里使用 const_cast 或 mutable 锁，或者简单返回
    return !write_buffer_.empty(); 
}

void Connection::HandleRead(const MessageCallback& cb) {
    if (state_ != ConnState::OPEN) return;

    char buf[4096];
    while (true) { // 必须循环读取
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            std::string data(buf, n);
            if (cb) cb(shared_from_this(), data);
        } else if (n == 0) {
            state_ = ConnState::CLOSED;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 数据读完了
            } else if (errno == EINTR) {
                continue; // 信号中断，继续
            } else {
                state_ = ConnState::CLOSED;
                break;
            }
        }
    }
}

void Connection::Send(const std::string& data) {
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (state_ != ConnState::OPEN) return;
        write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
    }
    TryFlushWriteBuffer();
}

void Connection::HandleWrite() {
    if (state_ == ConnState::CLOSED) return;
    TryFlushWriteBuffer();
}

bool Connection::TryFlushWriteBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (write_buffer_.empty()) return true;

    ssize_t n = ::write(fd_, write_buffer_.data(), write_buffer_.size());
    if (n > 0) {
        write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + n);
    } else if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            state_ = ConnState::CLOSED;
        }
    }
    return write_buffer_.empty();
}

void Connection::Close() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    state_ = ConnState::CLOSED;
}