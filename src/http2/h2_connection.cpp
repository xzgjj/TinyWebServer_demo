#include "http2/h2_connection.h"
#include "http2/h2_stream.h"
#include "Logger.h"
#include <cstring>

namespace tinywebserver {
namespace http2 {

namespace {
    // HTTP/2 客户端连接前言（RFC 7540 Section 3.5）
    constexpr char CLIENT_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    constexpr size_t CLIENT_PREFACE_LEN = 24;
} // namespace

H2Connection::H2Connection(int fd, bool is_server)
    : fd_(fd),
      is_server_(is_server),
      state_(H2ConnectionState::H2_IDLE),
      preface_remaining_(is_server ? CLIENT_PREFACE_LEN : 0) {

    LOG_INFO("HTTP/2 connection created fd=%d, is_server=%s",
             fd_, is_server ? "true" : "false");

    if (is_server) {
        // 服务器等待客户端前言
        read_buffer_.reserve(4096);
    } else {
        // 客户端需要发送前言
        // TODO: 发送客户端前言
        TransitionState(H2ConnectionState::H2_PREFACE_SENT, "Client connection");
    }
}

H2Connection::~H2Connection() {
    CloseAllStreams(Error(WebError::kInternalError, "Connection destroyed"));
    LOG_DEBUG("HTTP/2 connection destroyed fd=%d", fd_);
}

Error H2Connection::ProcessData(const uint8_t* data, size_t len) {
    if (IsClosed()) {
        return Error(WebError::kProtocolError, "Connection is closed");
    }

    // 如果还在等待前言，先处理前言
    if (state_ == H2ConnectionState::H2_IDLE && is_server_) {
        if (!ProcessPreface(data, len)) {
            // 前言不完整或错误
            return Error(WebError::kProtocolError, "Invalid HTTP/2 preface");
        }
        // 前言已处理，消耗前言字节
        size_t preface_consumed = CLIENT_PREFACE_LEN - preface_remaining_;
        data += preface_consumed;
        len -= preface_consumed;
    }

    // 将剩余数据添加到缓冲区
    read_buffer_.insert(read_buffer_.end(), data, data + len);

    // 处理缓冲区中的完整帧
    while (!read_buffer_.empty()) {
        if (read_buffer_.size() < 9) {
            // 帧头不完整，等待更多数据
            break;
        }

        // 解析帧头
        auto header_opt = H2FrameParser::ParseHeader(read_buffer_.data(), read_buffer_.size());
        if (!header_opt) {
            return Error(WebError::kProtocolError, "Failed to parse HTTP/2 frame header");
        }

        const auto& header = *header_opt;
        size_t frame_size = 9 + header.length;

        if (read_buffer_.size() < frame_size) {
            // 帧负载不完整，等待更多数据
            break;
        }

        // 处理完整帧
        const uint8_t* payload = read_buffer_.data() + 9;
        Error err = HandleFrame(header, payload);
        if (!err.IsSuccess()) {
            return err;
        }

        // 从缓冲区移除已处理的帧
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + frame_size);
    }

    return Error::Success();
}

bool H2Connection::ProcessPreface(const uint8_t* data, size_t len) {
    if (preface_remaining_ == 0) {
        return true; // 前言已处理完成
    }

    size_t to_compare = std::min(len, preface_remaining_);
    if (std::memcmp(data, CLIENT_PREFACE + (CLIENT_PREFACE_LEN - preface_remaining_), to_compare) != 0) {
        LOG_ERROR("Invalid HTTP/2 preface received");
        return false;
    }

    preface_remaining_ -= to_compare;
    LOG_DEBUG("HTTP/2 preface processed, remaining: %zu", preface_remaining_);

    if (preface_remaining_ == 0) {
        // 前言处理完成，发送 SETTINGS 帧并进入 OPEN 状态
        TransitionState(H2ConnectionState::H2_OPEN, "Preface completed");

        // 发送初始 SETTINGS 帧
        std::vector<std::pair<uint16_t, uint32_t>> initial_settings = {
            {1, settings_.header_table_size},
            {2, settings_.enable_push},
            {3, settings_.max_concurrent_streams},
            {4, settings_.initial_window_size},
            {5, settings_.max_frame_size},
            {6, settings_.max_header_list_size}
        };

        auto err = SendSettings(initial_settings);
        if (!err.IsSuccess()) {
            LOG_ERROR("Failed to send initial SETTINGS frame: %s", err.ToString().c_str());
        }
    }

    return true;
}

Error H2Connection::HandleFrame(const H2FrameHeader& header, const uint8_t* payload) {
    LOG_DEBUG("Handling HTTP/2 frame: type=%s, stream_id=%u, length=%u, flags=0x%02x",
              header.TypeName().c_str(), header.stream_id, header.length, header.flags);

    // 验证帧长度
    auto length_err = H2FrameParser::ValidatePayloadLength(header, header.length);
    if (!length_err.IsSuccess()) {
        return length_err;
    }

    // 根据帧类型分发处理
    switch (static_cast<H2FrameType>(header.type)) {
        case H2FrameType::DATA:
            return HandleDataFrame(header, payload);
        case H2FrameType::HEADERS:
            return HandleHeadersFrame(header, payload);
        case H2FrameType::SETTINGS:
            return HandleSettingsFrame(header, payload);
        case H2FrameType::PING:
            return HandlePingFrame(header, payload);
        case H2FrameType::GOAWAY:
            return HandleGoawayFrame(header, payload);
        case H2FrameType::WINDOW_UPDATE:
            return HandleWindowUpdateFrame(header, payload);
        case H2FrameType::RST_STREAM:
            return HandleRstStreamFrame(header, payload);
        case H2FrameType::PRIORITY:
            return HandlePriorityFrame(header, payload);
        case H2FrameType::PUSH_PROMISE:
            return HandlePushPromiseFrame(header, payload);
        case H2FrameType::CONTINUATION:
            return HandleContinuationFrame(header, payload);
        default:
            LOG_WARN("Unhandled HTTP/2 frame type: %s", header.TypeName().c_str());
            // 对于未知帧类型，根据 RFC 7540 Section 5.1，应忽略
            return Error::Success();
    }
}

Error H2Connection::SendFrame(H2FrameType type, uint8_t flags, uint32_t stream_id,
                              const std::vector<uint8_t>& payload) {
    if (IsClosed()) {
        return Error(WebError::kProtocolError, "Connection is closed");
    }

    H2FrameHeader header;
    header.length = static_cast<uint32_t>(payload.size());
    header.type = static_cast<uint8_t>(type);
    header.flags = flags;
    header.stream_id = stream_id;
    header.reserved = 0;

    auto err = header.Validate();
    if (!err.IsSuccess()) {
        return err;
    }

    // 序列化帧头
    auto header_bytes = H2FrameParser::SerializeHeader(header);

    // 通过回调发送帧
    if (frame_callback_) {
        frame_callback_(header, payload);
    } else {
        LOG_ERROR("No frame callback set for HTTP/2 connection fd=%d", fd_);
        return Error(WebError::kInternalError, "No frame callback");
    }

    LOG_DEBUG("Sent HTTP/2 frame: type=%s, stream_id=%u, length=%zu",
              header.TypeName().c_str(), stream_id, payload.size());

    return Error::Success();
}

Error H2Connection::SendSettings(const std::vector<std::pair<uint16_t, uint32_t>>& settings,
                                 bool ack) {
    std::vector<uint8_t> payload;
    payload.reserve(settings.size() * 6);

    for (const auto& [identifier, value] : settings) {
        payload.push_back((identifier >> 8) & 0xFF);
        payload.push_back(identifier & 0xFF);
        payload.push_back((value >> 24) & 0xFF);
        payload.push_back((value >> 16) & 0xFF);
        payload.push_back((value >> 8) & 0xFF);
        payload.push_back(value & 0xFF);
    }

    uint8_t flags = ack ? H2FrameFlags::ACK : 0;
    return SendFrame(H2FrameType::SETTINGS, flags, 0, payload);
}

Error H2Connection::SendGoaway(uint32_t last_stream_id, uint32_t error_code,
                               const std::vector<uint8_t>& debug_data) {
    std::vector<uint8_t> payload;
    payload.reserve(8 + debug_data.size());

    // 最后流ID（4字节）
    payload.push_back((last_stream_id >> 24) & 0xFF);
    payload.push_back((last_stream_id >> 16) & 0xFF);
    payload.push_back((last_stream_id >> 8) & 0xFF);
    payload.push_back(last_stream_id & 0xFF);

    // 错误码（4字节）
    payload.push_back((error_code >> 24) & 0xFF);
    payload.push_back((error_code >> 16) & 0xFF);
    payload.push_back((error_code >> 8) & 0xFF);
    payload.push_back(error_code & 0xFF);

    // 调试数据
    payload.insert(payload.end(), debug_data.begin(), debug_data.end());

    return SendFrame(H2FrameType::GOAWAY, 0, 0, payload);
}

Error H2Connection::SendPing(const std::array<uint8_t, 8>& opaque_data, bool ack) {
    std::vector<uint8_t> payload(opaque_data.begin(), opaque_data.end());
    uint8_t flags = ack ? H2FrameFlags::ACK : 0;
    return SendFrame(H2FrameType::PING, flags, 0, payload);
}

void H2Connection::Close(const Error& reason) {
    if (IsClosed()) {
        return;
    }

    LOG_INFO("Closing HTTP/2 connection fd=%d: %s", fd_, reason.ToString().c_str());

    // 发送 GOAWAY 帧（如果连接已打开）
    if (IsOpen()) {
        SendGoaway(0, 1, {}); // 使用默认错误码 NO_ERROR (0x0)
    }

    TransitionState(H2ConnectionState::H2_CLOSED, reason.ToString());

    // 关闭所有流
    CloseAllStreams(reason);

    // 通知上层
    if (close_callback_) {
        close_callback_(reason);
    }
}

size_t H2Connection::GetActiveStreamCount() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    return streams_.size();
}

void H2Connection::UpdateSettings(const H2Settings& new_settings) {
    settings_ = new_settings;
    LOG_DEBUG("Updated HTTP/2 connection settings");
}

void H2Connection::TransitionState(H2ConnectionState new_state, const std::string& reason) {
    H2ConnectionState current = state_.load(std::memory_order_acquire);
    if (current == new_state) {
        return;
    }

    if (!CanTransition(new_state)) {
        LOG_ERROR("Invalid HTTP/2 connection state transition: %d -> %d, reason: %s",
                  static_cast<int>(current), static_cast<int>(new_state), reason.c_str());
        return;
    }

    LOG_DEBUG("HTTP/2 connection state transition fd=%d: %d -> %d, reason: %s",
              fd_, static_cast<int>(current), static_cast<int>(new_state), reason.c_str());
    state_.store(new_state, std::memory_order_release);
}

bool H2Connection::CanTransition(H2ConnectionState new_state) const {
    H2ConnectionState current = state_.load(std::memory_order_acquire);

    // 简单状态转移规则
    switch (current) {
        case H2ConnectionState::H2_IDLE:
            return new_state == H2ConnectionState::H2_OPEN ||
                   new_state == H2ConnectionState::H2_CLOSED;
        case H2ConnectionState::H2_PREFACE_SENT:
            return new_state == H2ConnectionState::H2_OPEN ||
                   new_state == H2ConnectionState::H2_CLOSED;
        case H2ConnectionState::H2_OPEN:
            return new_state == H2ConnectionState::H2_CLOSING ||
                   new_state == H2ConnectionState::H2_CLOSED;
        case H2ConnectionState::H2_CLOSING:
            return new_state == H2ConnectionState::H2_CLOSED;
        case H2ConnectionState::H2_CLOSED:
            return false; // 已关闭状态不可转移
        default:
            return false;
    }
}

std::shared_ptr<H2Stream> H2Connection::GetOrCreateStream(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);

    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        return it->second;
    }

    // 创建新流
    auto stream = std::make_shared<H2Stream>(stream_id, this);
    streams_[stream_id] = stream;
    LOG_DEBUG("Created new HTTP/2 stream id=%u, total streams=%zu",
              stream_id, streams_.size());

    return stream;
}

void H2Connection::RemoveStream(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    streams_.erase(stream_id);
    LOG_DEBUG("Removed HTTP/2 stream id=%u, remaining streams=%zu",
              stream_id, streams_.size());
}

void H2Connection::CloseAllStreams([[maybe_unused]] const Error& reason) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto& [stream_id, stream] : streams_) {
        stream->Close(1); // 使用默认错误码 INTERNAL_ERROR
    }
    streams_.clear();
}

// 帧类型处理实现（占位符）
Error H2Connection::HandleDataFrame(const H2FrameHeader& header, [[maybe_unused]] const uint8_t* payload) {
    LOG_DEBUG("Handling DATA frame for stream %u", header.stream_id);
    // TODO: 实现 DATA 帧处理
    return Error::Success();
}

Error H2Connection::HandleHeadersFrame(const H2FrameHeader& header, [[maybe_unused]] const uint8_t* payload) {
    LOG_DEBUG("Handling HEADERS frame for stream %u", header.stream_id);
    // TODO: 实现 HEADERS 帧处理
    return Error::Success();
}

Error H2Connection::HandleSettingsFrame(const H2FrameHeader& header, const uint8_t* payload) {
    LOG_DEBUG("Handling SETTINGS frame, ACK=%s", header.HasFlag(H2FrameFlags::ACK) ? "true" : "false");

    if (header.HasFlag(H2FrameFlags::ACK)) {
        // ACK 帧，无需处理负载
        if (header.length != 0) {
            return Error(WebError::kProtocolError, "SETTINGS ACK frame must have zero length");
        }
        LOG_DEBUG("Received SETTINGS ACK");
        return Error::Success();
    }

    // 解析 SETTINGS 负载
    auto settings_opt = H2FrameParser::ParseSettingsPayload(payload, header.length);
    if (!settings_opt) {
        return Error(WebError::kProtocolError, "Invalid SETTINGS payload");
    }

    // 应用设置
    for (const auto& [identifier, value] : *settings_opt) {
        switch (identifier) {
            case 1: // SETTINGS_HEADER_TABLE_SIZE
                settings_.header_table_size = value;
                break;
            case 2: // SETTINGS_ENABLE_PUSH
                settings_.enable_push = value;
                break;
            case 3: // SETTINGS_MAX_CONCURRENT_STREAMS
                settings_.max_concurrent_streams = value;
                break;
            case 4: // SETTINGS_INITIAL_WINDOW_SIZE
                settings_.initial_window_size = value;
                break;
            case 5: // SETTINGS_MAX_FRAME_SIZE
                settings_.max_frame_size = value;
                break;
            case 6: // SETTINGS_MAX_HEADER_LIST_SIZE
                settings_.max_header_list_size = value;
                break;
            default:
                LOG_WARN("Unknown SETTINGS identifier: %u", identifier);
                // 根据 RFC 7540 Section 6.5.2，应忽略未知设置项
                break;
        }
    }

    // 发送 ACK
    return SendSettings({}, true);
}

Error H2Connection::HandlePingFrame(const H2FrameHeader& header, const uint8_t* payload) {
    LOG_DEBUG("Handling PING frame, ACK=%s", header.HasFlag(H2FrameFlags::ACK) ? "true" : "false");

    auto ping_data = H2FrameParser::ParsePingPayload(payload, header.length);
    if (!ping_data) {
        return Error(WebError::kProtocolError, "Invalid PING payload");
    }

    if (!header.HasFlag(H2FrameFlags::ACK)) {
        // 收到 PING 请求，发送 PING ACK
        return SendPing(*ping_data, true);
    }

    LOG_DEBUG("Received PING ACK");
    return Error::Success();
}

Error H2Connection::HandleGoawayFrame(const H2FrameHeader& header, const uint8_t* payload) {
    auto goaway_info = H2FrameParser::ParseGoawayPayload(payload, header.length);
    if (!goaway_info) {
        return Error(WebError::kProtocolError, "Invalid GOAWAY payload");
    }

    auto [last_stream_id, error_code] = *goaway_info;
    LOG_INFO("Received GOAWAY frame: last_stream_id=%u, error_code=%u",
             last_stream_id, error_code);

    // 进入关闭状态
    TransitionState(H2ConnectionState::H2_CLOSING, "Received GOAWAY");

    // 关闭所有流ID大于 last_stream_id 的流
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        for (auto it = streams_.begin(); it != streams_.end(); ) {
            if (it->first > last_stream_id) {
                it->second->Close(error_code);
                it = streams_.erase(it);
            } else {
                ++it;
            }
        }
    }

    return Error::Success();
}

Error H2Connection::HandleWindowUpdateFrame(const H2FrameHeader& header, [[maybe_unused]] const uint8_t* payload) {
    LOG_DEBUG("Handling WINDOW_UPDATE frame for stream %u", header.stream_id);
    // TODO: 实现流控制窗口更新
    return Error::Success();
}

Error H2Connection::HandleRstStreamFrame(const H2FrameHeader& header, const uint8_t* payload) {
    if (header.length != 4) {
        return Error(WebError::kProtocolError, "RST_STREAM frame must have 4-byte payload");
    }

    uint32_t error_code = (payload[0] << 24) | (payload[1] << 16) |
                          (payload[2] << 8) | payload[3];

    LOG_DEBUG("Handling RST_STREAM frame for stream %u, error_code=%u",
              header.stream_id, error_code);

    // 关闭指定流
    auto stream = GetOrCreateStream(header.stream_id);
    stream->Close(error_code);
    RemoveStream(header.stream_id);

    return Error::Success();
}

Error H2Connection::HandlePriorityFrame(const H2FrameHeader& header, [[maybe_unused]] const uint8_t* payload) {
    LOG_DEBUG("Handling PRIORITY frame for stream %u", header.stream_id);
    // TODO: 实现优先级处理
    return Error::Success();
}

Error H2Connection::HandlePushPromiseFrame(const H2FrameHeader& header, [[maybe_unused]] const uint8_t* payload) {
    LOG_DEBUG("Handling PUSH_PROMISE frame for stream %u", header.stream_id);
    // TODO: 实现服务器推送
    return Error::Success();
}

Error H2Connection::HandleContinuationFrame(const H2FrameHeader& header, [[maybe_unused]] const uint8_t* payload) {
    LOG_DEBUG("Handling CONTINUATION frame for stream %u", header.stream_id);
    // TODO: 实现 CONTINUATION 帧处理
    return Error::Success();
}

} // namespace http2
} // namespace tinywebserver