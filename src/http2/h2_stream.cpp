#include "http2/h2_stream.h"
#include "http2/h2_connection.h"
#include "Logger.h"

namespace tinywebserver {
namespace http2 {

H2Stream::H2Stream(uint32_t stream_id, H2Connection* connection)
    : stream_id_(stream_id),
      connection_(connection),
      state_(H2StreamState::IDLE),
      send_window_(65535),  // 默认初始窗口大小
      recv_window_(65535),
      data_callback_(nullptr),
      headers_callback_(nullptr),
      close_callback_(nullptr) {

    LOG_DEBUG("HTTP/2 stream created: id=%u", stream_id_);
}

H2Stream::~H2Stream() {
    LOG_DEBUG("HTTP/2 stream destroyed: id=%u", stream_id_);
}

Error H2Stream::HandleHeaders(const Headers& headers, bool end_stream) {
    LOG_DEBUG("Stream %u: Handling HEADERS, end_stream=%s, header_count=%zu",
              stream_id_, end_stream ? "true" : "false", headers.size());

    // 验证状态转移
    auto err = ValidateTransition(end_stream ? H2StreamState::HALF_CLOSED_REMOTE : H2StreamState::OPEN);
    if (!err.IsSuccess()) {
        return err;
    }

    // 保存请求头部
    request_headers_.insert(headers.begin(), headers.end());

    // 更新状态
    if (end_stream) {
        TransitionState(H2StreamState::HALF_CLOSED_REMOTE, "Received HEADERS with END_STREAM");
    } else {
        TransitionState(H2StreamState::OPEN, "Received HEADERS");
    }

    // 调用回调
    if (headers_callback_) {
        headers_callback_(headers, end_stream);
    }

    return Error::Success();
}

Error H2Stream::HandleData(const std::vector<uint8_t>& data, bool end_stream) {
    LOG_DEBUG("Stream %u: Handling DATA, size=%zu, end_stream=%s",
              stream_id_, data.size(), end_stream ? "true" : "false");

    // 验证状态转移
    if (end_stream) {
        auto err = ValidateTransition(H2StreamState::HALF_CLOSED_REMOTE);
        if (!err.IsSuccess()) {
            return err;
        }
    }

    // 添加到请求体
    request_body_.insert(request_body_.end(), data.begin(), data.end());

    // 更新状态
    if (end_stream) {
        TransitionState(H2StreamState::HALF_CLOSED_REMOTE, "Received DATA with END_STREAM");
    }

    // 调用回调
    if (data_callback_) {
        data_callback_(data, end_stream);
    }

    return Error::Success();
}

Error H2Stream::SendHeaders(const Headers& headers, bool end_stream) {
    LOG_DEBUG("Stream %u: Sending HEADERS, end_stream=%s", stream_id_, end_stream ? "true" : "false");

    // 验证状态转移
    auto err = ValidateTransition(end_stream ? H2StreamState::HALF_CLOSED_LOCAL : H2StreamState::OPEN);
    if (!err.IsSuccess()) {
        return err;
    }

    // 保存响应头部
    response_headers_.insert(headers.begin(), headers.end());

    // 更新状态
    if (end_stream) {
        TransitionState(H2StreamState::HALF_CLOSED_LOCAL, "Sent HEADERS with END_STREAM");
    } else {
        TransitionState(H2StreamState::OPEN, "Sent HEADERS");
    }

    // TODO: 实际发送 HEADERS 帧
    LOG_WARN("Stream %u: SendHeaders not fully implemented", stream_id_);

    return Error::Success();
}

Error H2Stream::SendData(const std::vector<uint8_t>& data, bool end_stream) {
    LOG_DEBUG("Stream %u: Sending DATA, size=%zu, end_stream=%s",
              stream_id_, data.size(), end_stream ? "true" : "false");

    // 验证状态转移
    if (end_stream) {
        auto err = ValidateTransition(H2StreamState::HALF_CLOSED_LOCAL);
        if (!err.IsSuccess()) {
            return err;
        }
    }

    // 添加到响应体
    response_body_.insert(response_body_.end(), data.begin(), data.end());

    // 更新状态
    if (end_stream) {
        TransitionState(H2StreamState::HALF_CLOSED_LOCAL, "Sent DATA with END_STREAM");
    }

    // TODO: 实际发送 DATA 帧
    LOG_WARN("Stream %u: SendData not fully implemented", stream_id_);

    return Error::Success();
}

Error H2Stream::SendRstStream(uint32_t error_code) {
    LOG_DEBUG("Stream %u: Sending RST_STREAM, error_code=%u", stream_id_, error_code);

    // 关闭流
    Close(error_code);

    // TODO: 实际发送 RST_STREAM 帧
    LOG_WARN("Stream %u: SendRstStream not fully implemented", stream_id_);

    return Error::Success();
}

void H2Stream::Close(uint32_t error_code) {
    if (IsClosed()) {
        return;
    }

    LOG_DEBUG("Stream %u: Closing, error_code=%u", stream_id_, error_code);
    TransitionState(H2StreamState::CLOSED, "Stream closed");

    // 调用关闭回调
    if (close_callback_) {
        close_callback_(error_code);
    }
}

Error H2Stream::UpdatePriority(const H2Priority& priority) {
    priority_ = priority;
    LOG_DEBUG("Stream %u: Updated priority", stream_id_);
    return Error::Success();
}

bool H2Stream::IsReadable() const {
    return state_ == H2StreamState::OPEN ||
           state_ == H2StreamState::HALF_CLOSED_LOCAL;
}

bool H2Stream::IsWritable() const {
    return state_ == H2StreamState::OPEN ||
           state_ == H2StreamState::HALF_CLOSED_REMOTE;
}

void H2Stream::TransitionState(H2StreamState new_state, const std::string& reason) {
    H2StreamState current = state_.load(std::memory_order_acquire);
    if (current == new_state) {
        return;
    }

    if (!CanTransition(new_state)) {
        LOG_ERROR("Invalid HTTP/2 stream state transition %u: %d -> %d, reason: %s",
                  stream_id_, static_cast<int>(current), static_cast<int>(new_state), reason.c_str());
        return;
    }

    LOG_DEBUG("HTTP/2 stream state transition %u: %d -> %d, reason: %s",
              stream_id_, static_cast<int>(current), static_cast<int>(new_state), reason.c_str());
    state_.store(new_state, std::memory_order_release);
}

bool H2Stream::CanTransition(H2StreamState new_state) const {
    H2StreamState current = state_.load(std::memory_order_acquire);

    // 简化状态转移规则（实际应遵循 RFC 7540 Section 5.1）
    switch (current) {
        case H2StreamState::IDLE:
            return new_state == H2StreamState::OPEN ||
                   new_state == H2StreamState::RESERVED_LOCAL ||
                   new_state == H2StreamState::RESERVED_REMOTE ||
                   new_state == H2StreamState::CLOSED;
        case H2StreamState::OPEN:
            return new_state == H2StreamState::HALF_CLOSED_LOCAL ||
                   new_state == H2StreamState::HALF_CLOSED_REMOTE ||
                   new_state == H2StreamState::CLOSED;
        case H2StreamState::HALF_CLOSED_LOCAL:
        case H2StreamState::HALF_CLOSED_REMOTE:
            return new_state == H2StreamState::CLOSED;
        case H2StreamState::RESERVED_LOCAL:
        case H2StreamState::RESERVED_REMOTE:
            return new_state == H2StreamState::HALF_CLOSED_REMOTE ||
                   new_state == H2StreamState::CLOSED;
        case H2StreamState::CLOSED:
            return false; // 已关闭状态不可转移
        default:
            return false;
    }
}

Error H2Stream::ValidateTransition(H2StreamState new_state) const {
    H2StreamState current = state_.load(std::memory_order_acquire);
    if (!CanTransition(new_state)) {
        return Error(WebError::kProtocolError,
                     "Invalid stream state transition: " +
                     std::to_string(static_cast<int>(current)) + " -> " +
                     std::to_string(static_cast<int>(new_state)));
    }
    return Error::Success();
}

} // namespace http2
} // namespace tinywebserver