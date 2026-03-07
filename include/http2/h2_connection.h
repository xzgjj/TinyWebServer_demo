#pragma once

#include <unordered_map>
#include <memory>
#include <vector>
#include <queue>
#include <atomic>
#include <mutex>
#include <functional>
#include "error/error.h"
#include "http2/h2_frame_parser.h"

namespace tinywebserver {
namespace http2 {

// 前向声明
class H2Stream;

/**
 * @brief HTTP/2 连接状态机状态
 */
enum class H2ConnectionState {
    H2_IDLE,           ///< 空闲状态，等待客户端前言
    H2_PREFACE_SENT,   ///< 已发送服务器前言
    H2_OPEN,           ///< 连接已打开，可以处理帧
    H2_CLOSING,        ///< 正在关闭（GOAWAY 已发送）
    H2_CLOSED          ///< 连接已关闭
};

/**
 * @brief HTTP/2 连接设置项
 */
struct H2Settings {
    // 默认值（RFC 7540 Section 6.5.2）
    static constexpr uint32_t HEADER_TABLE_SIZE = 4096;        // 设置项1
    static constexpr uint32_t ENABLE_PUSH = 1;                 // 设置项2
    static constexpr uint32_t MAX_CONCURRENT_STREAMS = 100;    // 设置项3
    static constexpr uint32_t INITIAL_WINDOW_SIZE = 65535;     // 设置项4
    static constexpr uint32_t MAX_FRAME_SIZE = 16384;          // 设置项5
    static constexpr uint32_t MAX_HEADER_LIST_SIZE = 65536;    // 设置项6

    uint32_t header_table_size = HEADER_TABLE_SIZE;
    uint32_t enable_push = ENABLE_PUSH;
    uint32_t max_concurrent_streams = MAX_CONCURRENT_STREAMS;
    uint32_t initial_window_size = INITIAL_WINDOW_SIZE;
    uint32_t max_frame_size = MAX_FRAME_SIZE;
    uint32_t max_header_list_size = MAX_HEADER_LIST_SIZE;
};

/**
 * @brief HTTP/2 连接类
 *
 * 管理 HTTP/2 连接状态机，处理帧分发，管理流状态。
 */
class H2Connection {
public:
    using FrameCallback = std::function<void(const H2FrameHeader& header,
                                             const std::vector<uint8_t>& payload)>;
    using CloseCallback = std::function<void(const Error& reason)>;

    /**
     * @brief 构造函数
     * @param fd 连接文件描述符
     * @param is_server 是否为服务器端连接
     */
    H2Connection(int fd, bool is_server = true);
    ~H2Connection();

    // 禁止拷贝
    H2Connection(const H2Connection&) = delete;
    H2Connection& operator=(const H2Connection&) = delete;

    /**
     * @brief 处理接收到的数据
     * @param data 原始字节数据
     * @param len 数据长度
     * @return Error 对象，如果处理成功则返回 Success()
     */
    Error ProcessData(const uint8_t* data, size_t len);

    /**
     * @brief 处理单个 HTTP/2 帧
     * @param header 帧头
     * @param payload 帧负载
     * @return Error 对象
     */
    Error HandleFrame(const H2FrameHeader& header, const uint8_t* payload);

    /**
     * @brief 发送帧
     * @param type 帧类型
     * @param flags 帧标志
     * @param stream_id 流标识符
     * @param payload 帧负载
     * @return Error 对象
     */
    Error SendFrame(H2FrameType type, uint8_t flags, uint32_t stream_id,
                    const std::vector<uint8_t>& payload);

    /**
     * @brief 发送 SETTINGS 帧
     * @param settings 设置项列表
     * @param ack 是否为 ACK 帧
     * @return Error 对象
     */
    Error SendSettings(const std::vector<std::pair<uint16_t, uint32_t>>& settings,
                       bool ack = false);

    /**
     * @brief 发送 GOAWAY 帧
     * @param last_stream_id 最后处理的流ID
     * @param error_code 错误码
     * @param debug_data 调试数据
     * @return Error 对象
     */
    Error SendGoaway(uint32_t last_stream_id, uint32_t error_code,
                     const std::vector<uint8_t>& debug_data = {});

    /**
     * @brief 发送 PING 帧
     * @param opaque_data 8字节不透明数据
     * @param ack 是否为 ACK 帧
     * @return Error 对象
     */
    Error SendPing(const std::array<uint8_t, 8>& opaque_data, bool ack = false);

    /**
     * @brief 处理连接前言（客户端必须发送 "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"）
     * @param data 前言数据
     * @param len 数据长度
     * @return 处理成功返回 true，数据不足或前言错误返回 false
     */
    bool ProcessPreface(const uint8_t* data, size_t len);

    /**
     * @brief 获取连接状态
     */
    H2ConnectionState GetState() const { return state_; }

    /**
     * @brief 获取连接是否已打开
     */
    bool IsOpen() const { return state_ == H2ConnectionState::H2_OPEN; }

    /**
     * @brief 获取连接是否已关闭
     */
    bool IsClosed() const { return state_ == H2ConnectionState::H2_CLOSED; }

    /**
     * @brief 关闭连接
     * @param reason 关闭原因
     */
    void Close(const Error& reason);

    /**
     * @brief 获取活跃流数量
     */
    size_t GetActiveStreamCount() const;

    /**
     * @brief 获取连接设置
     */
    const H2Settings& GetSettings() const { return settings_; }

    /**
     * @brief 更新连接设置
     */
    void UpdateSettings(const H2Settings& new_settings);

    /**
     * @brief 设置帧回调（用于向外发送帧）
     */
    void SetFrameCallback(FrameCallback cb) { frame_callback_ = std::move(cb); }

    /**
     * @brief 设置关闭回调
     */
    void SetCloseCallback(CloseCallback cb) { close_callback_ = std::move(cb); }

    /**
     * @brief 获取文件描述符
     */
    int GetFd() const { return fd_; }

    /**
     * @brief 获取是否为服务器端
     */
    bool IsServer() const { return is_server_; }

private:
    // 内部帧处理方法
    Error HandleDataFrame(const H2FrameHeader& header, const uint8_t* payload);
    Error HandleHeadersFrame(const H2FrameHeader& header, const uint8_t* payload);
    Error HandleSettingsFrame(const H2FrameHeader& header, const uint8_t* payload);
    Error HandlePingFrame(const H2FrameHeader& header, const uint8_t* payload);
    Error HandleGoawayFrame(const H2FrameHeader& header, const uint8_t* payload);
    Error HandleWindowUpdateFrame(const H2FrameHeader& header, const uint8_t* payload);
    Error HandleRstStreamFrame(const H2FrameHeader& header, const uint8_t* payload);
    Error HandlePriorityFrame(const H2FrameHeader& header, const uint8_t* payload);
    Error HandlePushPromiseFrame(const H2FrameHeader& header, const uint8_t* payload);
    Error HandleContinuationFrame(const H2FrameHeader& header, const uint8_t* payload);

    // 状态转移
    void TransitionState(H2ConnectionState new_state, const std::string& reason);
    bool CanTransition(H2ConnectionState new_state) const;

    // 流管理
    std::shared_ptr<H2Stream> GetOrCreateStream(uint32_t stream_id);
    void RemoveStream(uint32_t stream_id);
    void CloseAllStreams(const Error& reason);

    int fd_;
    bool is_server_;
    std::atomic<H2ConnectionState> state_;

    H2Settings settings_;
    std::unordered_map<uint32_t, std::shared_ptr<H2Stream>> streams_;
    mutable std::mutex streams_mutex_;

    // 帧处理缓冲区（用于处理跨帧的数据）
    std::vector<uint8_t> read_buffer_;
    size_t preface_remaining_;

    FrameCallback frame_callback_;
    CloseCallback close_callback_;
};

} // namespace http2
} // namespace tinywebserver