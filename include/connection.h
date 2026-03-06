//

#ifndef CONNECTION_H
#define CONNECTION_H

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <atomic>

#include "http_request.h"
#include "buffer_chain.h"
#include "static_resource_manager.h" // for StaticResource
#include "connection_limits.h"
#include "error/error.h"


class HttpRequest; 
class EventLoop;
class StaticResource; // 确保 StaticResource 也能被识别

enum class ConnState {
    kConnecting,      ///< 连接建立中 (SYN_SENT)
    kConnected,       ///< 连接已建立，等待请求
    kReading,         ///< 读取请求中
    kProcessing,      ///< 业务处理中 (生成响应)
    kWriting,         ///< 发送响应中
    kClosing,         ///< 正在关闭 (半关闭)
    kClosed           ///< 完全关闭
};

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using MessageCallback = std::function<void(std::shared_ptr<Connection>, const std::string&)>;
    using CloseCallback = std::function<void(int)>;

    Connection(int fd, EventLoop* loop);
    ~Connection();

    // 初始化连接，必须在 loop 线程调用
    void ConnectEstablished();
    
    // 发送数据接口 (线程安全)
    void Send(const std::string& data);
    void Send(const char* data, size_t len);
    // 【新增】零拷贝发送静态资源
    void Send(std::shared_ptr<StaticResource> resource);

    // 关闭连接 (线程安全)
    void Shutdown();

    // 状态与属性
    int GetFd() const { return fd_; }
    EventLoop* GetLoop() const { return loop_; }
    /// 检查连接是否处于活动状态（可进行读写操作）
    bool IsConnected() const {
        return state_ != ConnState::kClosed && state_ != ConnState::kClosing && state_ != ConnState::kConnecting;
    }

    void SetMessageCallback(MessageCallback cb) { message_callback_ = std::move(cb); }
    void SetCloseCallback(CloseCallback cb) { close_callback_ = std::move(cb); }

    // HTTP 相关辅助
    std::string& GetInputBuffer() { return input_buffer_; }
    std::shared_ptr<HttpRequest> GetHttpParser() { return http_parser_; }

    // 【新增】清空读缓冲区 (用于长连接复用)
    void ClearReadBuffer();

    // 【新增】超时管理
    void SetReadTimeout(int seconds);
    void SetWriteTimeout(int seconds);
    void SetIdleTimeout(int seconds);
    void ResetIdleTimeout();
    void DisableAllTimeouts();
    bool HasActiveTimeout() const;

    // 【新增】统一关闭路径
    void Close(const tinywebserver::Error& reason);

private:
    void HandleRead(int fd);
    void HandleWrite(int fd);
    void HandleClose(int fd, const tinywebserver::Error& reason = tinywebserver::Error::Success());
    void HandleError(int fd);
    
    void SendInLoop(const std::string& data);
    void SendResourceInLoop(std::shared_ptr<StaticResource> res);
    void ShutdownInLoop();

    EventLoop* loop_;
    int fd_;
    std::atomic<ConnState> state_;

    /// 检查状态转移是否允许
    bool CanTransition(ConnState new_state) const;
    /// 执行状态转移（带日志和验证）
    void Transition(ConnState new_state, const std::string& reason = "");

    /// 检查输入缓冲区是否超过限制
    bool CheckInputBufferLimit() const;
    /// 检查输出缓冲区是否超过限制
    bool CheckOutputBufferLimit() const;
    /// 暂停读取（背压机制）
    void PauseReading();
    /// 恢复读取
    void ResumeReading();

    /// 【新增】超时管理
    void SetupTimeout(int seconds, const std::string& timeout_type);
    void CancelTimeout(const std::string& timeout_type);
    void OnTimeout(const std::string& timeout_type);
    void UpdateActivityTimestamp();
    void CheckAndSetTimeouts();

    // 【新增】统一关闭路径内部实现
    void CloseInLoop(const tinywebserver::Error& reason);

    // 读缓冲区：依然保持 string，处理 HTTP 文本协议头
    std::string input_buffer_;
    
    // 【修改】写缓冲区：升级为支持 Scatter/Gather 的链式缓冲
    BufferChain output_buffer_;

    // 【新增】超时管理
    int read_timeout_seconds_;
    int write_timeout_seconds_;
    int idle_timeout_seconds_;
    bool read_timeout_active_;
    bool write_timeout_active_;
    bool idle_timeout_active_;
    std::chrono::steady_clock::time_point last_activity_time_;

    std::shared_ptr<HttpRequest> http_parser_;
    MessageCallback message_callback_;
    CloseCallback close_callback_;
};

#endif