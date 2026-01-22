//

#include "connection.h"
#include "http_request.h" 
#include "server_metrics.h"
#include "reactor/event_loop.h"
#include "Logger.h"

#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h> // writev


Connection::Connection(int fd, EventLoop* loop)
    : loop_(loop), fd_(fd), state_(ConnState::kConnecting),
      http_parser_(new HttpRequest()) {
}

Connection::~Connection() {
    LOG_DEBUG("Connection dtor fd=%d", fd_);
    ::close(fd_);
}

void Connection::ConnectEstablished() {
    if (!loop_->IsInLoopThread()) {
        LOG_FATAL("ConnectEstablished must be called in loop thread");
    }
    
    state_ = ConnState::kConnected;
    std::weak_ptr<Connection> weak_self(shared_from_this());
    
    loop_->SetReadCallback(fd_, [weak_self](int fd){
        if (auto self = weak_self.lock()) self->HandleRead(fd);
    });
    
    loop_->SetWriteCallback(fd_, [weak_self](int fd){
        if (auto self = weak_self.lock()) self->HandleWrite(fd);
    });

    loop_->UpdateEvent(fd_, EPOLLIN | EPOLLET);
}

void Connection::Send(const char* data, size_t len) {
    Send(std::string(data, len));
}

void Connection::Send(const std::string& data) {
    if (state_ != ConnState::kConnected) return;

    if (loop_->IsInLoopThread()) {
        SendInLoop(data);
    } else {
        loop_->RunInLoop([self = shared_from_this(), data]() { 
            self->SendInLoop(data); 
        });
    }
}

void Connection::Send(std::shared_ptr<StaticResource> resource) {
    if (state_ != ConnState::kConnected || !resource) return;

    if (loop_->IsInLoopThread()) {
        SendResourceInLoop(resource);
    } else {
        loop_->RunInLoop([self = shared_from_this(), resource]() { 
            self->SendResourceInLoop(resource); 
        });
    }
}

void Connection::SendInLoop(const std::string& data) {
    if (data.empty()) return;
    output_buffer_.Append(data);
    // 尝试直接触发一次写操作，尽快将数据发出去
    // 只有当之前没有注册 EPOLLOUT 时才尝试直接写，避免乱序
    // 这里简化逻辑：直接调用 HandleWrite，它会处理好 writev 和事件注册
    HandleWrite(fd_); 
}

void Connection::SendResourceInLoop(std::shared_ptr<StaticResource> res) {
    if (!res || res->size == 0) return;
    output_buffer_.Append(res);
    HandleWrite(fd_);
}

void Connection::HandleRead(int fd) {
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            input_buffer_.append(buf, n);
        } else if (n == 0) {
            HandleClose(fd);
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_ERROR("HandleRead error on fd=%d, err=%d", fd, errno);
            HandleError(fd);
            break;
        }
    }
    if (!input_buffer_.empty() && message_callback_) {
        message_callback_(shared_from_this(), input_buffer_);
    }
}

// 【核心重构】支持聚集写和断点续传
void Connection::HandleWrite(int fd)
{
    if (state_ == ConnState::kDisconnected) return;

    struct iovec iov[16]; 
    // 修改处：output_buffer_chain_ -> output_buffer_
    int count = output_buffer_.GetIov(iov, 16);
    
    if (count > 0)
    {
        ssize_t n = ::writev(fd, iov, count);
        if (n > 0)
        {
            // 修改处：output_buffer_chain_ -> output_buffer_
            output_buffer_.Advance(static_cast<size_t>(n));
            ServerMetrics::GetInstance().OnBytesSent(static_cast<size_t>(n));

            if (output_buffer_.IsEmpty())
            {
                loop_->UpdateEvent(fd_, EPOLLIN | EPOLLET);
                if (state_ == ConnState::kDisconnecting) ShutdownInLoop();
            }
            else
            {
                loop_->UpdateEvent(fd_, EPOLLIN | EPOLLOUT | EPOLLET);
            }
        }
        // ... 后续逻辑 ...
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            loop_->UpdateEvent(fd_, EPOLLIN | EPOLLOUT | EPOLLET);
        }
        else
        {
            LOG_ERROR("HandleWrite fatal error on fd %d", fd);
            HandleError(fd);
        }
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
    
    // 只有当缓冲区为空时才真正关闭写端
    if (output_buffer_.IsEmpty()) {
        ::shutdown(fd_, SHUT_WR);
    } else {
        // 还有数据没发完，HandleWrite 发完后会再次调用 ShutdownInLoop
        LOG_INFO("Shutdown pending, waiting buffer drain... fd=%d", fd_);
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
    HandleClose(fd);
}