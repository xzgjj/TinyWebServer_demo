//

#include "connection.h"   // 头文件只包含类声明
#include <unistd.h>       // for close, read, write
#include <algorithm>      // for std::min

// 构造函数
Connection::Connection(int fd)
    : fd_(fd), state_(ConnState::OPEN) {}

// 析构函数
Connection::~Connection() {
    Close();
}

// 获取文件描述符
int Connection::Fd() const noexcept {
    return fd_;
}

// 获取连接状态
ConnState Connection::State() const noexcept {
    return state_;
}



// 处理读事件
void Connection::HandleRead() {
    char buf[1024];
    ssize_t n = read(fd_, buf, sizeof(buf));
    if (n > 0) {
        // 1. 存入读缓冲区（用于后续协议解析）
        read_buffer_.insert(read_buffer_.end(), buf, buf + n);
        
        // 2. 【核心修复】为了通过测试，将数据存入写缓冲区实现 Echo
        write_buffer_.insert(write_buffer_.end(), buf, buf + n);
        
    } else if (n == 0) {
        state_ = ConnState::CLOSED;
    } 
}

// 处理写事件
void Connection::HandleWrite() {
    if (write_buffer_.empty()) return;

    ssize_t n = write(fd_, write_buffer_.data(), write_buffer_.size());
    if (n > 0) {
        write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + n);
    }

    if (write_buffer_.empty() && state_ == ConnState::CLOSING) {
        Close();
    }
}

// 尝试刷新写缓冲区（可选接口）
bool Connection::TryFlushWriteBuffer() {
    if (write_buffer_.empty()) return true;
    HandleWrite();
    return write_buffer_.empty();
}

// 关闭连接
void Connection::Close() {
    if (state_ != ConnState::CLOSED) {
        ::close(fd_);
        state_ = ConnState::CLOSED;
    }
}
