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
    
    Transition(ConnState::kConnected, "connection established");
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
    if (!IsConnected()) return;

    if (loop_->IsInLoopThread()) {
        SendInLoop(data);
    } else {
        loop_->RunInLoop([self = shared_from_this(), data]() { 
            self->SendInLoop(data); 
        });
    }
}

void Connection::Send(std::shared_ptr<StaticResource> resource) {
    if (!IsConnected() || !resource) return;

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
    // 输出缓冲区边界检查（包含待添加数据）
    size_t new_size = output_buffer_.TotalBytes() + data.size();
    if (ConnectionLimits::IsOutputBufferExceeded(new_size)) {
        LOG_ERROR("Output buffer limit exceeded (current=%zu + new=%zu > limit=%zu), closing connection fd=%d",
                  output_buffer_.TotalBytes(), data.size(),
                  ConnectionLimits::kMaxOutputBuffer, fd_);
        HandleClose(fd_);
        return;
    }
    output_buffer_.Append(data);
    // 尝试直接触发一次写操作，尽快将数据发出去
    // 只有当之前没有注册 EPOLLOUT 时才尝试直接写，避免乱序
    // 这里简化逻辑：直接调用 HandleWrite，它会处理好 writev 和事件注册
    HandleWrite(fd_);
}

void Connection::SendResourceInLoop(std::shared_ptr<StaticResource> res) {
    if (!res || res->size == 0) return;
    // 输出缓冲区边界检查（包含待添加资源大小）
    size_t new_size = output_buffer_.TotalBytes() + res->size;
    if (ConnectionLimits::IsOutputBufferExceeded(new_size)) {
        LOG_ERROR("Output buffer limit exceeded (current=%zu + resource=%zu > limit=%zu), closing connection fd=%d",
                  output_buffer_.TotalBytes(), res->size,
                  ConnectionLimits::kMaxOutputBuffer, fd_);
        HandleClose(fd_);
        return;
    }
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
    // 背压检查：如果输入缓冲区超过限制，暂停读取
    if (CheckInputBufferLimit()) {
        PauseReading();
    }
    if (!input_buffer_.empty() && message_callback_) {
        message_callback_(shared_from_this(), input_buffer_);
    }
}

// 【核心重构】支持聚集写和断点续传
void Connection::HandleWrite(int fd)
{
    if (state_ == ConnState::kClosed) return;

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
                if (state_ == ConnState::kClosing) ShutdownInLoop();
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
    if (IsConnected()) {
        Transition(ConnState::kClosing, "shutdown requested");
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
    Transition(ConnState::kClosed, "connection closed");
    loop_->RemoveEvent(fd);
    if (close_callback_) {
        close_callback_(fd);
    }
}

void Connection::HandleError(int fd) {
    HandleClose(fd);
}
// 状态转移实现
bool Connection::CanTransition(ConnState new_state) const {
    ConnState current = state_.load(std::memory_order_acquire);
    // 相同状态允许（无操作）
    if (current == new_state) return true;

    // 关闭状态是终止状态，不允许转移出去
    if (current == ConnState::kClosed) return false;

    // 允许任何状态转移到 kClosing（错误/主动关闭路径）
    if (new_state == ConnState::kClosing) return true;

    // 允许任何状态转移到 kClosed（强制关闭路径）
    if (new_state == ConnState::kClosed) return true;

    // 正常业务流程转移规则
    switch (current) {
        case ConnState::kConnecting:
            return new_state == ConnState::kConnected;
        case ConnState::kConnected:
            return new_state == ConnState::kReading;
        case ConnState::kReading:
            return new_state == ConnState::kProcessing;
        case ConnState::kProcessing:
            return new_state == ConnState::kWriting;
        case ConnState::kWriting:
            return new_state == ConnState::kConnected; // 保持连接复用
        case ConnState::kClosing:
            // 只能转移到 kClosed（已在上面处理）
            return false;
        default:
            return false;
    }
}

void Connection::Transition(ConnState new_state, const std::string& reason) {
    ConnState current = state_.load(std::memory_order_acquire);
    if (!CanTransition(new_state)) {
        LOG_ERROR("Invalid state transition from %d to %d, reason: %s", 
                  static_cast<int>(current), static_cast<int>(new_state), reason.c_str());
        // 对于无效转移，强制关闭连接
        if (current != ConnState::kClosed && current != ConnState::kClosing) {
            state_ = ConnState::kClosing;
            // 异步关闭
            loop_->RunInLoop([self = shared_from_this()]() {
                self->HandleClose(self->fd_);
            });
        }
        return;
    }
    LOG_DEBUG("State transition fd=%d: %d -> %d, reason: %s", 
              fd_, static_cast<int>(current), static_cast<int>(new_state), reason.c_str());
    state_.store(new_state, std::memory_order_release);
}

// 资源限制检查实现
bool Connection::CheckInputBufferLimit() const {
    size_t current_size = input_buffer_.size();
    bool exceeded = ConnectionLimits::IsInputBufferExceeded(current_size);
    if (exceeded) {
        LOG_WARN("Input buffer limit exceeded: %zu > %zu, fd=%d", 
                 current_size, ConnectionLimits::kMaxInputBuffer, fd_);
    }
    return exceeded;
}

bool Connection::CheckOutputBufferLimit() const {
    size_t current_size = output_buffer_.TotalBytes();
    bool exceeded = ConnectionLimits::IsOutputBufferExceeded(current_size);
    if (exceeded) {
        LOG_WARN("Output buffer limit exceeded: %zu > %zu, fd=%d",
                 current_size, ConnectionLimits::kMaxOutputBuffer, fd_);
    }
    return exceeded;
}

void Connection::PauseReading() {
    // 从 epoll 事件中移除 EPOLLIN，暂停读取
    loop_->UpdateEvent(fd_, EPOLLOUT | EPOLLET); // 只保留写事件
    LOG_DEBUG("Pause reading on fd=%d due to backpressure", fd_);
}

void Connection::ResumeReading() {
    // 恢复 EPOLLIN 事件
    loop_->UpdateEvent(fd_, EPOLLIN | EPOLLOUT | EPOLLET);
    LOG_DEBUG("Resume reading on fd=%d", fd_);
}

void Connection::ClearReadBuffer() {
    input_buffer_.clear();
    // 清空缓冲区后恢复读取（如果之前因背压暂停）
    ResumeReading();
    LOG_DEBUG("Read buffer cleared, fd=%d", fd_);
}
