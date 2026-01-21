//

#include "connection.h"
#include "reactor/event_loop.h"
#include "Logger.h"
#include <unistd.h>
#include <sys/socket.h>

Connection::Connection(int fd, EventLoop* loop)
    : loop_(loop), fd_(fd), state_(ConnState::kConnecting),
      http_parser_(new HttpRequest()) {
}

Connection::~Connection() {
    LOG_DEBUG("Connection dtor fd=%d", fd_);
    close(fd_);
}

void Connection::ConnectEstablished() {
    if (!loop_->IsInLoopThread()) {
        LOG_FATAL("ConnectEstablished must be called in loop thread");
    }
    
    state_ = ConnState::kConnected;
    
    std::weak_ptr<Connection> weak_self(shared_from_this());
    
    // 修改点 1: 传入 fd_
    loop_->SetReadCallback(fd_, [weak_self](int fd){
        if (auto self = weak_self.lock()) self->HandleRead(fd);
    });
    
    loop_->SetWriteCallback(fd_, [weak_self](int fd){
        if (auto self = weak_self.lock()) self->HandleWrite(fd);
    });

    // 开启读事件 (ET 模式)
    loop_->UpdateEvent(fd_, EPOLLIN | EPOLLET);
}

void Connection::Send(const std::string& data) {
    if (state_ != ConnState::kConnected) return;

    if (loop_->IsInLoopThread()) {
        SendInLoop(data);
    } else {
        // 跨线程调用：将发送任务转移到 IO 线程
        loop_->RunInLoop(
            [self = shared_from_this(), data]() { 
                self->SendInLoop(data); 
            }
        );
    }
}

void Connection::SendInLoop(const std::string& data) {
    ssize_t nwrote = 0;
    size_t remaining = data.size();
    bool fault_error = false;

    // 1. 如果当前缓冲区为空，尝试直接写入 socket
    if (output_buffer_.empty()) {
        nwrote = write(fd_, data.data(), remaining);
        if (nwrote >= 0) {
            remaining -= nwrote;
            if (remaining == 0) {
                // 写完了，无需关注 EPOLLOUT
            }
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK) {
                LOG_ERROR("SendInLoop write error");
                if (errno == EPIPE || errno == ECONNRESET) {
                    fault_error = true;
                }
            }
        }
    }

    // 2. 如果没写完，追加到缓冲区并关注 EPOLLOUT
    if (!fault_error && remaining > 0) {
        output_buffer_.append(data.data() + nwrote, remaining);
        // 注册写事件
        loop_->UpdateEvent(fd_, EPOLLIN | EPOLLOUT | EPOLLET);
    }
}

void Connection::HandleRead(int fd) {
    char buf[4096];
    bool error = false;
    while (true) { // ET 模式核心修复：循环读
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            input_buffer_.append(buf, n);
        } else if (n == 0) {
            HandleClose(fd);
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 数据已读尽
            }
            LOG_ERROR("HandleRead error on fd=%d", fd);
            HandleError(fd);
            break;
        }
    }

    if (!input_buffer_.empty() && message_callback_) {
        message_callback_(shared_from_this(), input_buffer_);
    }
}

void Connection::HandleWrite(int fd) {
    if (output_buffer_.empty()) {
        // 只有当之前关注了写事件时，才重置为只读
        // 建议在 Connection 中记录当前 events 状态以做对比
        loop_->UpdateEvent(fd_, EPOLLIN | EPOLLET); 
        return;
    }

    ssize_t n = write(fd, output_buffer_.data(), output_buffer_.size());
    if (n > 0) {
        output_buffer_.erase(0, n);
        if (output_buffer_.empty()) {
            // 写完，取消关注 EPOLLOUT
            loop_->UpdateEvent(fd_, EPOLLIN | EPOLLET);
        }
    } else {
        LOG_ERROR("HandleWrite error");
    }
}

void Connection::Shutdown() {
    if (state_ == ConnState::kConnected) {
        state_ = ConnState::kDisconnecting;
        loop_->RunInLoop(std::bind(&Connection::ShutdownInLoop, this));
    }
}

void Connection::ShutdownInLoop() {
    if (!loop_->IsInLoopThread()) return;
    // 如果没有数据要发，直接关闭写端
    if (output_buffer_.empty()) {
        shutdown(fd_, SHUT_WR);
    }
}

void Connection::HandleClose(int fd) {
    state_ = ConnState::kDisconnected;
    loop_->RemoveEvent(fd);
    if (close_callback_) {
        close_callback_(fd);
    }
}

void Connection::HandleError(int fd) {
    LOG_ERROR("Connection error fd=%d err=%d", fd, errno);
    HandleClose(fd);
}