#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <functional>
#include "error/error.h"

namespace tinywebserver {
namespace http2 {

// 前向声明
class H2Connection;

/**
 * @brief HTTP/2 流状态（RFC 7540 Section 5.1）
 */
enum class H2StreamState {
    IDLE,                 ///< 空闲状态
    RESERVED_LOCAL,       ///< 本地保留（已发送 PUSH_PROMISE）
    RESERVED_REMOTE,      ///< 远程保留（已接收 PUSH_PROMISE）
    OPEN,                 ///< 流已打开
    HALF_CLOSED_LOCAL,    ///< 本地半关闭（已发送 END_STREAM）
    HALF_CLOSED_REMOTE,   ///< 远程半关闭（已接收 END_STREAM）
    CLOSED                ///< 流已关闭
};

/**
 * @brief HTTP/2 流优先级信息（RFC 7540 Section 5.3）
 */
struct H2Priority {
    bool exclusive = false;
    uint32_t stream_dependency = 0;
    uint8_t weight = 16;  // 默认权重 16 (值 1-256)
};

/**
 * @brief HTTP/2 流类
 *
 * 管理单个 HTTP/2 流的生命周期，包括状态机、请求/响应头和数据处理。
 */
class H2Stream {
public:
    using Headers = std::unordered_map<std::string, std::string>;
    using DataCallback = std::function<void(const std::vector<uint8_t>& data, bool end_stream)>;
    using HeadersCallback = std::function<void(const Headers& headers, bool end_stream)>;
    using CloseCallback = std::function<void(uint32_t error_code)>;

    /**
     * @brief 构造函数
     * @param stream_id 流标识符
     * @param connection 所属连接
     */
    H2Stream(uint32_t stream_id, H2Connection* connection);
    ~H2Stream();

    // 禁止拷贝
    H2Stream(const H2Stream&) = delete;
    H2Stream& operator=(const H2Stream&) = delete;

    /**
     * @brief 处理 HEADERS 帧
     * @param headers 头部块
     * @param end_stream 是否结束流
     * @return Error 对象
     */
    Error HandleHeaders(const Headers& headers, bool end_stream);

    /**
     * @brief 处理 DATA 帧
     * @param data 数据负载
     * @param end_stream 是否结束流
     * @return Error 对象
     */
    Error HandleData(const std::vector<uint8_t>& data, bool end_stream);

    /**
     * @brief 发送 HEADERS 帧
     * @param headers 头部映射表
     * @param end_stream 是否结束流
     * @return Error 对象
     */
    Error SendHeaders(const Headers& headers, bool end_stream);

    /**
     * @brief 发送 DATA 帧
     * @param data 数据
     * @param end_stream 是否结束流
     * @return Error 对象
     */
    Error SendData(const std::vector<uint8_t>& data, bool end_stream);

    /**
     * @brief 发送 RST_STREAM 帧
     * @param error_code 错误码
     * @return Error 对象
     */
    Error SendRstStream(uint32_t error_code);

    /**
     * @brief 关闭流
     * @param error_code 错误码（0表示正常关闭）
     */
    void Close(uint32_t error_code = 0);

    /**
     * @brief 获取流状态
     */
    H2StreamState GetState() const { return state_; }

    /**
     * @brief 获取流标识符
     */
    uint32_t GetStreamId() const { return stream_id_; }

    /**
     * @brief 获取请求头部
     */
    const Headers& GetRequestHeaders() const { return request_headers_; }

    /**
     * @brief 获取响应头部
     */
    const Headers& GetResponseHeaders() const { return response_headers_; }

    /**
     * @brief 获取已接收的请求体数据
     */
    const std::vector<uint8_t>& GetRequestBody() const { return request_body_; }

    /**
     * @brief 获取优先级信息
     */
    const H2Priority& GetPriority() const { return priority_; }

    /**
     * @brief 更新优先级信息
     * @param priority 新的优先级
     * @return Error 对象
     */
    Error UpdatePriority(const H2Priority& priority);

    /**
     * @brief 检查流是否可读（远程未关闭）
     */
    bool IsReadable() const;

    /**
     * @brief 检查流是否可写（本地未关闭）
     */
    bool IsWritable() const;

    /**
     * @brief 检查流是否已关闭
     */
    bool IsClosed() const { return state_ == H2StreamState::CLOSED; }

    /**
     * @brief 设置数据回调（接收到 DATA 帧时调用）
     */
    void SetDataCallback(DataCallback cb) { data_callback_ = std::move(cb); }

    /**
     * @brief 设置头部回调（接收到 HEADERS 帧时调用）
     */
    void SetHeadersCallback(HeadersCallback cb) { headers_callback_ = std::move(cb); }

    /**
     * @brief 设置关闭回调
     */
    void SetCloseCallback(CloseCallback cb) { close_callback_ = std::move(cb); }

private:
    // 状态转移
    void TransitionState(H2StreamState new_state, const std::string& reason);
    bool CanTransition(H2StreamState new_state) const;

    // 验证状态转移
    Error ValidateTransition(H2StreamState new_state) const;

    uint32_t stream_id_;
    H2Connection* connection_;
    std::atomic<H2StreamState> state_;

    H2Priority priority_;
    Headers request_headers_;
    Headers response_headers_;
    std::vector<uint8_t> request_body_;
    std::vector<uint8_t> response_body_;

    // 流控制窗口
    int32_t send_window_;
    int32_t recv_window_;

    DataCallback data_callback_;
    HeadersCallback headers_callback_;
    CloseCallback close_callback_;
};

} // namespace http2
} // namespace tinywebserver