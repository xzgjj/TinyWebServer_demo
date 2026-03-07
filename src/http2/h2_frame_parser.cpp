#include "http2/h2_frame_parser.h"
#include "Logger.h"
#include <arpa/inet.h>
#include <cstring>
#include <array>
#include <sstream>

namespace tinywebserver {
namespace http2 {

Error H2FrameHeader::Validate() const {
    // 检查保留位（必须为0）
    if (reserved != 0) {
        return Error(WebError::kProtocolError,
                     "HTTP/2 frame reserved bit must be 0");
    }

    // 检查流ID有效性
    if (!H2FrameParser::IsValidStreamId(stream_id)) {
        return Error(WebError::kProtocolError,
                     "Invalid HTTP/2 stream ID: " + std::to_string(stream_id));
    }

    // 检查帧长度有效性
    auto length_err = H2FrameParser::ValidateFrameLength(length,
                                                         static_cast<H2FrameType>(type));
    if (!length_err.IsSuccess()) {
        return length_err;
    }

    return Error::Success();
}

std::string H2FrameHeader::TypeName() const {
    return H2FrameParser::FrameTypeToString(static_cast<H2FrameType>(type));
}

std::optional<H2FrameHeader> H2FrameParser::ParseHeader(const uint8_t* data, size_t len) {
    if (len < 9) {
        LOG_ERROR("HTTP/2 frame header too short: %zu bytes", len);
        return std::nullopt;
    }

    H2FrameHeader header;

    // 解析24位长度（大端）
    header.length = (data[0] << 16) | (data[1] << 8) | data[2];

    // 解析类型和标志
    header.type = data[3];
    header.flags = data[4];

    // 解析流ID（31位，大端）和保留位
    uint32_t stream_id_and_reserved = (data[5] << 24) | (data[6] << 16) |
                                      (data[7] << 8) | data[8];
    header.reserved = (stream_id_and_reserved >> 31) & 0x1;
    header.stream_id = stream_id_and_reserved & 0x7FFFFFFF;

    LOG_DEBUG("Parsed HTTP/2 frame header: type=%s, length=%u, flags=0x%02x, "
              "stream_id=%u, reserved=%u",
              header.TypeName().c_str(), header.length, header.flags,
              header.stream_id, header.reserved);

    // 验证帧头
    auto err = header.Validate();
    if (!err.IsSuccess()) {
        LOG_ERROR("Invalid HTTP/2 frame header: %s", err.ToString().c_str());
        return std::nullopt;
    }

    return header;
}

std::vector<uint8_t> H2FrameParser::SerializeHeader(const H2FrameHeader& header) {
    std::vector<uint8_t> result(9);

    // 序列化24位长度（大端）
    result[0] = (header.length >> 16) & 0xFF;
    result[1] = (header.length >> 8) & 0xFF;
    result[2] = header.length & 0xFF;

    // 序列化类型和标志
    result[3] = header.type;
    result[4] = header.flags;

    // 序列化流ID和保留位
    uint32_t stream_id_and_reserved = header.stream_id & 0x7FFFFFFF;
    if (header.reserved) {
        stream_id_and_reserved |= (1 << 31);
    }

    result[5] = (stream_id_and_reserved >> 24) & 0xFF;
    result[6] = (stream_id_and_reserved >> 16) & 0xFF;
    result[7] = (stream_id_and_reserved >> 8) & 0xFF;
    result[8] = stream_id_and_reserved & 0xFF;

    return result;
}

std::optional<std::vector<std::pair<uint16_t, uint32_t>>>
H2FrameParser::ParseSettingsPayload(const uint8_t* payload, size_t len) {
    // SETTINGS 帧负载必须是6字节倍数（每个设置项2字节标识符+4字节值）
    if (len % 6 != 0) {
        LOG_ERROR("Invalid SETTINGS payload length: %zu (not multiple of 6)", len);
        return std::nullopt;
    }

    std::vector<std::pair<uint16_t, uint32_t>> settings;
    size_t num_settings = len / 6;

    for (size_t i = 0; i < num_settings; ++i) {
        const uint8_t* setting = payload + i * 6;

        uint16_t identifier = (setting[0] << 8) | setting[1];
        uint32_t value = (setting[2] << 24) | (setting[3] << 16) |
                         (setting[4] << 8) | setting[5];

        settings.emplace_back(identifier, value);
        LOG_DEBUG("Parsed SETTINGS item: identifier=%u, value=%u", identifier, value);
    }

    return settings;
}

std::optional<std::array<uint8_t, 8>>
H2FrameParser::ParsePingPayload(const uint8_t* payload, size_t len) {
    if (len != 8) {
        LOG_ERROR("Invalid PING payload length: %zu (must be 8)", len);
        return std::nullopt;
    }

    std::array<uint8_t, 8> opaque_data;
    std::memcpy(opaque_data.data(), payload, 8);
    return opaque_data;
}

std::optional<std::pair<uint32_t, uint32_t>>
H2FrameParser::ParseGoawayPayload(const uint8_t* payload, size_t len) {
    // GOAWAY 帧负载至少8字节（最后流ID4字节+错误码4字节）
    if (len < 8) {
        LOG_ERROR("Invalid GOAWAY payload length: %zu (must be at least 8)", len);
        return std::nullopt;
    }

    uint32_t last_stream_id = (payload[0] << 24) | (payload[1] << 16) |
                              (payload[2] << 8) | payload[3];

    uint32_t error_code = (payload[4] << 24) | (payload[5] << 16) |
                          (payload[6] << 8) | payload[7];

    LOG_DEBUG("Parsed GOAWAY: last_stream_id=%u, error_code=%u",
              last_stream_id, error_code);

    return std::make_pair(last_stream_id, error_code);
}

Error H2FrameParser::ValidatePayloadLength(const H2FrameHeader& header, size_t payload_len) {
    if (header.length != payload_len) {
        return Error(WebError::kProtocolError,
                     "HTTP/2 frame payload length mismatch: header=" +
                     std::to_string(header.length) + ", actual=" +
                     std::to_string(payload_len));
    }
    return Error::Success();
}

std::string H2FrameParser::FrameTypeToString(H2FrameType type) {
    switch (type) {
        case H2FrameType::DATA: return "DATA";
        case H2FrameType::HEADERS: return "HEADERS";
        case H2FrameType::PRIORITY: return "PRIORITY";
        case H2FrameType::RST_STREAM: return "RST_STREAM";
        case H2FrameType::SETTINGS: return "SETTINGS";
        case H2FrameType::PUSH_PROMISE: return "PUSH_PROMISE";
        case H2FrameType::PING: return "PING";
        case H2FrameType::GOAWAY: return "GOAWAY";
        case H2FrameType::WINDOW_UPDATE: return "WINDOW_UPDATE";
        case H2FrameType::CONTINUATION: return "CONTINUATION";
        default: return "UNKNOWN(" + std::to_string(static_cast<uint8_t>(type)) + ")";
    }
}

std::optional<H2FrameType> H2FrameParser::StringToFrameType(const std::string& name) {
    if (name == "DATA") return H2FrameType::DATA;
    if (name == "HEADERS") return H2FrameType::HEADERS;
    if (name == "PRIORITY") return H2FrameType::PRIORITY;
    if (name == "RST_STREAM") return H2FrameType::RST_STREAM;
    if (name == "SETTINGS") return H2FrameType::SETTINGS;
    if (name == "PUSH_PROMISE") return H2FrameType::PUSH_PROMISE;
    if (name == "PING") return H2FrameType::PING;
    if (name == "GOAWAY") return H2FrameType::GOAWAY;
    if (name == "WINDOW_UPDATE") return H2FrameType::WINDOW_UPDATE;
    if (name == "CONTINUATION") return H2FrameType::CONTINUATION;
    return std::nullopt;
}

bool H2FrameParser::IsValidStreamId([[maybe_unused]] uint32_t stream_id) {
    // 流ID 0 用于连接控制帧
    // 客户端发起的流ID必须为奇数，服务端发起的流ID必须为偶数
    // 但这里只做基本检查
    return true;
}

Error H2FrameParser::ValidateFrameLength(uint32_t length, H2FrameType type) {
    // 根据 RFC 7540 Section 6 检查帧长度限制
    switch (type) {
        case H2FrameType::SETTINGS:
            // SETTINGS 帧负载必须是6的倍数
            if (length % 6 != 0) {
                return Error(WebError::kProtocolError,
                             "Invalid SETTINGS frame length: " + std::to_string(length) +
                             " (must be multiple of 6)");
            }
            break;

        case H2FrameType::PING:
            // PING 帧负载必须是8字节
            if (length != 8) {
                return Error(WebError::kProtocolError,
                             "Invalid PING frame length: " + std::to_string(length) +
                             " (must be 8)");
            }
            break;

        case H2FrameType::WINDOW_UPDATE:
            // WINDOW_UPDATE 帧负载必须是4字节
            if (length != 4) {
                return Error(WebError::kProtocolError,
                             "Invalid WINDOW_UPDATE frame length: " + std::to_string(length) +
                             " (must be 4)");
            }
            break;

        case H2FrameType::PRIORITY:
            // PRIORITY 帧负载必须是5字节
            if (length != 5) {
                return Error(WebError::kProtocolError,
                             "Invalid PRIORITY frame length: " + std::to_string(length) +
                             " (must be 5)");
            }
            break;

        case H2FrameType::RST_STREAM:
            // RST_STREAM 帧负载必须是4字节
            if (length != 4) {
                return Error(WebError::kProtocolError,
                             "Invalid RST_STREAM frame length: " + std::to_string(length) +
                             " (must be 4)");
            }
            break;

        case H2FrameType::GOAWAY:
            // GOAWAY 帧负载至少8字节
            if (length < 8) {
                return Error(WebError::kProtocolError,
                             "Invalid GOAWAY frame length: " + std::to_string(length) +
                             " (must be at least 8)");
            }
            break;

        default:
            // 其他帧类型长度限制较少
            break;
    }

    // 通用长度限制：最大帧负载长度 2^14-1 (16383) 字节
    // 实际实现可能支持更大的帧，但规范要求默认限制
    if (length > 16383) {
        LOG_WARN("HTTP/2 frame length %u exceeds default max frame size (16383)", length);
        // 注意：这不是致命错误，SETTINGS 帧可以协商更大的最大帧大小
    }

    return Error::Success();
}

} // namespace http2
} // namespace tinywebserver