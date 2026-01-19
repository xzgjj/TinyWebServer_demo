//

//
#include "connection.h"
#include "reactor/epoll_reactor.h"
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <cstring>
#include <iostream>

Connection::Connection(int fd, EpollReactor* reactor) 
    : fd_(fd), 
      reactor_(reactor), 
      state_(ConnState::OPEN), 
      http_parser_(std::make_shared<HttpRequest>()) {
    // 构造函数体内可以留空，或者进行简单的日志记录
}

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

// 修复后的 Send (业务层调用)
void Connection::Send(const std::string& data) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (state_ != ConnState::OPEN) return;
    
    output_buffer_.append(data);
    
    // 【关键修复】通知 Reactor 关注可写事件
    // 如果不 MOD 为 EPOLLOUT，HandleWrite 永远不会执行，导致 TIMEOUT
    reactor_->UpdateEvent(fd_, EPOLLIN | EPOLLOUT | EPOLLET);
}

// 修复后的 Send (供 Reactor 的 HandleWrite 调用)
int Connection::Send() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (output_buffer_.empty()) {
        // 数据发完了，去掉 EPOLLOUT 关注，防止忙轮询
        reactor_->UpdateEvent(fd_, EPOLLIN | EPOLLET);
        return 0;
    }
    
    ssize_t n = ::send(fd_, output_buffer_.data(), output_buffer_.size(), 0);
    if (n > 0) {
        output_buffer_.erase(0, n);
        if (output_buffer_.empty()) {
            reactor_->UpdateEvent(fd_, EPOLLIN | EPOLLET);
        }
    } else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
        Close();
    }
    return static_cast<int>(n);
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
        needs_epoll_update_ = true; // 状态从"有数据"变"无数据"，需要 MOD 去掉 EPOLLOUT
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

int Connection::Recv() {
    char buf[4096];
    int total_read = 0;
    while (true) {
        ssize_t n = recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            input_buffer_.append(buf, n);
            total_read += n;
        } else if (n == 0) {
            return 0; // 对端关闭
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 读完了
            return -1; // 出错
        }
    }
    return total_read;
}

