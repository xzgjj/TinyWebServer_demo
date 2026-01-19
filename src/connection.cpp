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
    if (fd_ >= 0) ::close(fd_);
}

void Connection::HandleRead(const MessageCallback& cb) {
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            input_buffer_.append(buf, n);
            if (cb) cb(shared_from_this(), "");
        } else if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            Close(); break;
        } else { // n == 0
            Close(); break;
        }
    }
}

void Connection::Send(const std::string& data) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (state_ != ConnState::OPEN) return;
    
    bool was_empty = !HasPendingWrite();
    write_buffer_.append(data);
    
    // 如果之前是空的，现在有数据了，可能需要让 Reactor 监听 EPOLLOUT
    if (was_empty) {
        needs_epoll_update_ = true;
    }
    
    // 立即尝试发送一次
    TryFlushWriteBuffer();
}

bool Connection::TryFlushWriteBuffer() {
    // 假设调用者已持有 buffer_mutex_ 或在单线程 Reactor 循环中
    if (!HasPendingWrite()) return true;

    while (write_offset_ < write_buffer_.size()) {
        ssize_t n = ::write(fd_, write_buffer_.data() + write_offset_, 
                           write_buffer_.size() - write_offset_);
        if (n > 0) {
            write_offset_ += n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                write_blocked_ = true;
                return false; 
            }
            if (errno == EINTR) continue;
            Close(); return false;
        }
    }

    // 发送完毕，重置缓冲区
    if (write_offset_ == write_buffer_.size()) {
        write_buffer_.clear();
        write_offset_ = 0;
        write_blocked_ = false;
        needs_epoll_update_ = true; // 状态从“有数据”变“无数据”，需要 MOD 去掉 EPOLLOUT
    }
    return true;
}

void Connection::HandleWrite() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    TryFlushWriteBuffer();
}

void Connection::Close() {
    state_ = ConnState::CLOSED;
    // 此处不关 fd，由析构函数负责
}