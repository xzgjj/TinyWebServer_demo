//

#include "connection.h"
#include "http_request.h"
#include "server_metrics.h"
#include "reactor/event_loop.h"
#include "Logger.h"
#include "http/keep_alive_manager.h"

using namespace tinywebserver;

#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h> // writev


Connection::Connection(int fd, EventLoop* loop,
                       std::shared_ptr<tinywebserver::ServerConfig> config,
                       tinywebserver::KeepAliveManager* keep_alive_manager)
    : loop_(loop), fd_(fd), state_(ConnState::kConnecting),
      config_(config),
      keep_alive_manager_(keep_alive_manager),
      read_timeout_seconds_(0),
      write_timeout_seconds_(0),
      idle_timeout_seconds_(0),
      read_timeout_active_(false),
      write_timeout_active_(false),
      idle_timeout_active_(false),
      last_activity_time_(std::chrono::steady_clock::now()),
      http_parser_(new HttpRequest()) {

    // 从配置设置超时和限制
    if (config_) {
        auto limits = config_->GetLimitsOptions();
        read_timeout_seconds_ = limits.connection_timeout;
        write_timeout_seconds_ = limits.connection_timeout; // 使用相同超时，或可配置
        idle_timeout_seconds_ = limits.keep_alive_timeout;
    }
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
    size_t max_limit = ConnectionLimits::kMaxOutputBuffer;
    if (config_) {
        auto limits = config_->GetLimitsOptions();
        max_limit = limits.max_output_buffer;
    }
    if (new_size > max_limit) {
        LOG_ERROR("Output buffer limit exceeded (current=%zu + new=%zu > limit=%zu), closing connection fd=%d",
                  output_buffer_.TotalBytes(), data.size(),
                  max_limit, fd_);
        HandleClose(fd_, tinywebserver::Error(tinywebserver::WebError::kTimeout, "connection timeout"));
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
    size_t max_limit = ConnectionLimits::kMaxOutputBuffer;
    if (config_) {
        auto limits = config_->GetLimitsOptions();
        max_limit = limits.max_output_buffer;
    }
    if (new_size > max_limit) {
        LOG_ERROR("Output buffer limit exceeded (current=%zu + resource=%zu > limit=%zu), closing connection fd=%d",
                  output_buffer_.TotalBytes(), res->size,
                  max_limit, fd_);
        HandleClose(fd_, tinywebserver::Error(tinywebserver::WebError::kTimeout, "connection timeout"));
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
            UpdateActivityTimestamp();  // 更新活动时间戳
        } else if (n == 0) {
            HandleClose(fd, tinywebserver::Error::Success());
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
            UpdateActivityTimestamp();  // 更新活动时间戳

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

void Connection::HandleClose(int fd, const tinywebserver::Error& reason) {
    // 记录关闭原因（如果不是成功）
    if (reason.IsFailure()) {
        LOG_INFO("Connection fd=%d closed with error: %s", fd, reason.ToString().c_str());
    } else {
        LOG_DEBUG("Connection fd=%d closed normally", fd);
    }

    Transition(ConnState::kClosed, "connection closed");
    loop_->RemoveEvent(fd);
    // 通知 Keep-Alive 管理器连接关闭
    if (keep_alive_manager_) {
        keep_alive_manager_->OnConnectionClose(fd);
    }
    if (close_callback_) {
        close_callback_(fd);
    }
}

void Connection::HandleError(int fd) {
    // 使用通用内部错误
    HandleClose(fd, tinywebserver::Error(tinywebserver::WebError::kInternalError, "socket error"));
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
    // 状态变化时更新超时设置
    CheckAndSetTimeouts();
}

// 资源限制检查实现
bool Connection::CheckInputBufferLimit() const {
    size_t current_size = input_buffer_.size();
    size_t max_limit = ConnectionLimits::kMaxInputBuffer; // 默认值
    if (config_) {
        auto limits = config_->GetLimitsOptions();
        max_limit = limits.max_input_buffer;
    }
    bool exceeded = current_size > max_limit;
    if (exceeded) {
        LOG_WARN("Input buffer limit exceeded: %zu > %zu, fd=%d",
                 current_size, max_limit, fd_);
    }
    return exceeded;
}

bool Connection::CheckOutputBufferLimit() const {
    size_t current_size = output_buffer_.TotalBytes();
    size_t max_limit = ConnectionLimits::kMaxOutputBuffer; // 默认值
    if (config_) {
        auto limits = config_->GetLimitsOptions();
        max_limit = limits.max_output_buffer;
    }
    bool exceeded = current_size > max_limit;
    if (exceeded) {
        LOG_WARN("Output buffer limit exceeded: %zu > %zu, fd=%d",
                 current_size, max_limit, fd_);
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

// ============================================================================
// 超时管理实现
// ============================================================================

void Connection::SetReadTimeout(int seconds) {
    if (seconds <= 0) {
        read_timeout_seconds_ = 0;
        CancelTimeout("read");
    } else {
        read_timeout_seconds_ = seconds;
        CheckAndSetTimeouts();
    }
}

void Connection::SetWriteTimeout(int seconds) {
    if (seconds <= 0) {
        write_timeout_seconds_ = 0;
        CancelTimeout("write");
    } else {
        write_timeout_seconds_ = seconds;
        CheckAndSetTimeouts();
    }
}

void Connection::SetIdleTimeout(int seconds) {
    if (seconds <= 0) {
        idle_timeout_seconds_ = 0;
        CancelTimeout("idle");
    } else {
        idle_timeout_seconds_ = seconds;
        CheckAndSetTimeouts();
    }
}

void Connection::ResetIdleTimeout() {
    UpdateActivityTimestamp();
    if (idle_timeout_seconds_ > 0) {
        SetupTimeout(idle_timeout_seconds_, "idle");
    }
}

void Connection::DisableAllTimeouts() {
    read_timeout_seconds_ = 0;
    write_timeout_seconds_ = 0;
    idle_timeout_seconds_ = 0;
    CancelTimeout("read");
    CancelTimeout("write");
    CancelTimeout("idle");
}

bool Connection::HasActiveTimeout() const {
    return read_timeout_active_ || write_timeout_active_ || idle_timeout_active_;
}

void Connection::SetupTimeout(int seconds, const std::string& timeout_type) {
    if (seconds <= 0) {
        return;
    }

    if (!loop_->IsInLoopThread()) {
        loop_->RunInLoop([self = shared_from_this(), seconds, timeout_type]() {
            self->SetupTimeout(seconds, timeout_type);
        });
        return;
    }

    // 创建超时回调
    auto callback = [self = shared_from_this(), timeout_type]() {
        self->OnTimeout(timeout_type);
    };

    // 添加定时器
    auto error = loop_->AddTimer(fd_, seconds, std::move(callback));
    if (error.IsFailure()) {
        LOG_ERROR("Failed to setup %s timeout for fd=%d: %s",
                 timeout_type.c_str(), fd_, error.ToString().c_str());
    } else {
        // 更新活动状态
        if (timeout_type == "read") {
            read_timeout_active_ = true;
        } else if (timeout_type == "write") {
            write_timeout_active_ = true;
        } else if (timeout_type == "idle") {
            idle_timeout_active_ = true;
        }
        LOG_DEBUG("Set %s timeout for fd=%d: %d seconds",
                 timeout_type.c_str(), fd_, seconds);
    }
}

void Connection::CancelTimeout(const std::string& timeout_type) {
    if (!loop_->IsInLoopThread()) {
        loop_->RunInLoop([self = shared_from_this(), timeout_type]() {
            self->CancelTimeout(timeout_type);
        });
        return;
    }

    // 移除定时器
    auto error = loop_->RemoveTimer(fd_);
    if (error.IsFailure() && error.GetCode() != tinywebserver::WebError::kSuccess) {
        LOG_WARN("Failed to cancel %s timeout for fd=%d: %s",
                timeout_type.c_str(), fd_, error.ToString().c_str());
    } else {
        // 更新活动状态
        if (timeout_type == "read") {
            read_timeout_active_ = false;
        } else if (timeout_type == "write") {
            write_timeout_active_ = false;
        } else if (timeout_type == "idle") {
            idle_timeout_active_ = false;
        }
        LOG_DEBUG("Cancelled %s timeout for fd=%d", timeout_type.c_str(), fd_);
    }
}

void Connection::OnTimeout(const std::string& timeout_type) {
    if (!loop_->IsInLoopThread()) {
        loop_->RunInLoop([self = shared_from_this(), timeout_type]() {
            self->OnTimeout(timeout_type);
        });
        return;
    }

    LOG_WARN("Connection timeout fd=%d: %s timeout", fd_, timeout_type.c_str());

    // 更新活动状态
    if (timeout_type == "read") {
        read_timeout_active_ = false;
    } else if (timeout_type == "write") {
        write_timeout_active_ = false;
    } else if (timeout_type == "idle") {
        idle_timeout_active_ = false;
    }

    // 根据超时类型处理
    if (timeout_type == "read") {
        // 读超时：关闭连接
        HandleClose(fd_, tinywebserver::Error(tinywebserver::WebError::kTimeout, "connection timeout"));
    } else if (timeout_type == "write") {
        // 写超时：关闭连接
        HandleClose(fd_, tinywebserver::Error(tinywebserver::WebError::kTimeout, "connection timeout"));
    } else if (timeout_type == "idle") {
        // 空闲超时：关闭连接
        HandleClose(fd_, tinywebserver::Error(tinywebserver::WebError::kTimeout, "connection timeout"));
    }
}

void Connection::UpdateActivityTimestamp() {
    last_activity_time_ = std::chrono::steady_clock::now();
    // 重置空闲超时
    if (idle_timeout_seconds_ > 0) {
        SetupTimeout(idle_timeout_seconds_, "idle");
    }
}

void Connection::CheckAndSetTimeouts() {
    // 根据当前状态和配置设置超时
    // 这是一个简化的实现，实际可能需要更精细的控制
    ConnState current = state_.load(std::memory_order_acquire);

    // 读超时：在Reading状态时生效
    if (read_timeout_seconds_ > 0 && current == ConnState::kReading) {
        SetupTimeout(read_timeout_seconds_, "read");
    } else {
        CancelTimeout("read");
    }

    // 写超时：在Writing状态时生效
    if (write_timeout_seconds_ > 0 && current == ConnState::kWriting) {
        SetupTimeout(write_timeout_seconds_, "write");
    } else {
        CancelTimeout("write");
    }

    // 空闲超时：在Connected状态时生效
    if (idle_timeout_seconds_ > 0 && current == ConnState::kConnected) {
        SetupTimeout(idle_timeout_seconds_, "idle");
    } else {
        CancelTimeout("idle");
    }
}

// ============================================================================
// 统一关闭路径实现
// ============================================================================

void Connection::Close(const tinywebserver::Error& reason) {
    if (!IsConnected()) {
        // 连接已经关闭，忽略
        return;
    }

    LOG_INFO("Closing connection fd=%d: %s", fd_, reason.ToString().c_str());

    if (loop_->IsInLoopThread()) {
        CloseInLoop(reason);
    } else {
        loop_->RunInLoop([self = shared_from_this(), reason]() {
            self->CloseInLoop(reason);
        });
    }
}

void Connection::CloseInLoop(const tinywebserver::Error& reason) {
    if (!loop_->IsInLoopThread()) {
        return;
    }

    ConnState current = state_.load(std::memory_order_acquire);
    if (current == ConnState::kClosed || current == ConnState::kClosing) {
        // 已经在关闭过程中，忽略重复关闭
        LOG_DEBUG("Connection fd=%d already in closing state %d", fd_, static_cast<int>(current));
        return;
    }

    // 记录关闭原因
    LOG_INFO("Connection fd=%d closing with reason: %s", fd_, reason.ToString().c_str());

    // 禁用所有超时
    DisableAllTimeouts();

    // 根据错误类型决定关闭策略
    if (reason.GetSysErrno() != 0) {
        // 系统错误：立即关闭
        HandleClose(fd_, tinywebserver::Error(tinywebserver::WebError::kTimeout, "connection timeout"));
    } else if (reason.GetCode() == tinywebserver::WebError::kTimeout) {
        // 超时错误：立即关闭
        HandleClose(fd_, tinywebserver::Error(tinywebserver::WebError::kTimeout, "connection timeout"));
    } else if (reason.GetCode() == tinywebserver::WebError::kBufferFull ||
               reason.GetCode() == tinywebserver::WebError::kMemoryLimit) {
        // 资源限制错误：尝试优雅关闭
        Transition(ConnState::kClosing, "resource limit reached");
        ShutdownInLoop();
    } else {
        // 其他错误：优雅关闭
        Transition(ConnState::kClosing, "error: " + reason.GetMessage());
        ShutdownInLoop();
    }
}

// ============================================================================
// Keep-Alive 管理实现
// ============================================================================

void Connection::UpdateKeepAliveState(bool keep_alive, int idle_timeout) {
    if (!keep_alive_manager_) {
        return;
    }
    keep_alive_manager_->OnRequestStart(fd_, keep_alive, idle_timeout);
}

void Connection::OnRequestStart(bool keep_alive, int idle_timeout) {
    UpdateKeepAliveState(keep_alive, idle_timeout);
}

void Connection::OnRequestComplete() {
    if (!keep_alive_manager_) {
        return;
    }
    keep_alive_manager_->OnRequestComplete(fd_);
}

bool Connection::ShouldKeepAlive() const {
    if (!keep_alive_manager_) {
        return false;
    }
    auto state = keep_alive_manager_->GetConnectionState(fd_);
    return state && state->keep_alive;
}
