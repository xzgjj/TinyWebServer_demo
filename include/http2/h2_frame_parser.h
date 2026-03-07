#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <array>
#include "error/error.h"

namespace tinywebserver {
namespace http2 {

/**
 * @brief HTTP/2 帧类型 (RFC 7540 Section 6.1)
 */
enum class H2FrameType : uint8_t {
    DATA = 0x0,
    HEADERS = 0x1,
    PRIORITY = 0x2,
    RST_STREAM = 0x3,
    SETTINGS = 0x4,
    PUSH_PROMISE = 0x5,
    PING = 0x6,
    GOAWAY = 0x7,
    WINDOW_UPDATE = 0x8,
    CONTINUATION = 0x9
};

/**
 * @brief HTTP/2 帧标志位 (RFC 7540 Section 6.2)
 */
enum H2FrameFlags : uint8_t {
    END_STREAM = 0x1,
    ACK = 0x1,
    END_HEADERS = 0x4,
    PADDED = 0x8,
    PRIORITY = 0x20
};

/**
 * @brief HTTP/2 帧头（9字节）
 *
 * 网络字节序（大端）：
 * +-----------------------------------------------+
 * |                 Length (24)                   |
 * +---------------+---------------+---------------+
 * |   Type (8)    |   Flags (8)   |
 * +-+-------------+---------------+-------------------------------+
 * |R|                 Stream Identifier (31)                      |
 * +-+-------------------------------------------------------------+
 */
struct H2FrameHeader {
    uint32_t length : 24;      ///< 帧负载长度（不包括帧头）
    uint8_t type;              ///< 帧类型
    uint8_t flags;             ///< 帧标志
    uint32_t stream_id : 31;   ///< 流标识符
    uint8_t reserved : 1;      ///< 保留位（必须为0）

    /**
     * @brief 检查帧头是否有效
     * @return Error 对象，如果有效则返回 Success()
     */
    Error Validate() const;

    /**
     * @brief 获取帧类型名称
     * @return 帧类型字符串表示
     */
    std::string TypeName() const;

    /**
     * @brief 检查是否设置了特定标志
     * @param flag 标志位
     * @return true 如果标志被设置
     */
    bool HasFlag(uint8_t flag) const {
        return (flags & flag) == flag;
    }
};

/**
 * @brief HTTP/2 帧解析器
 *
 * 负责解析 HTTP/2 帧头和帧负载，提供帧的序列化和反序列化功能。
 */
class H2FrameParser {
public:
    /**
     * @brief 解析帧头
     * @param data 原始字节数据（至少9字节）
     * @param len 数据长度
     * @return 解析后的帧头，如果解析失败则返回 std::nullopt
     */
    static std::optional<H2FrameHeader> ParseHeader(const uint8_t* data, size_t len);

    /**
     * @brief 序列化帧头
     * @param header 帧头
     * @return 序列化后的9字节数据
     */
    static std::vector<uint8_t> SerializeHeader(const H2FrameHeader& header);

    /**
     * @brief 解析 SETTINGS 帧负载
     * @param payload 帧负载数据
     * @param len 负载长度
     * @return 设置项列表（键值对），如果解析失败则返回 std::nullopt
     */
    static std::optional<std::vector<std::pair<uint16_t, uint32_t>>>
    ParseSettingsPayload(const uint8_t* payload, size_t len);

    /**
     * @brief 解析 PING 帧负载
     * @param payload 帧负载数据
     * @param len 负载长度（必须为8）
     * @return 8字节的 opaque data，如果解析失败则返回 std::nullopt
     */
    static std::optional<std::array<uint8_t, 8>>
    ParsePingPayload(const uint8_t* payload, size_t len);

    /**
     * @brief 解析 GOAWAY 帧负载
     * @param payload 帧负载数据
     * @param len 负载长度
     * @return GOAWAY 信息（最后流ID和错误码），如果解析失败则返回 std::nullopt
     */
    static std::optional<std::pair<uint32_t, uint32_t>>
    ParseGoawayPayload(const uint8_t* payload, size_t len);

    /**
     * @brief 检查帧负载长度是否与帧头匹配
     * @param header 帧头
     * @param payload_len 实际负载长度
     * @return Error 对象，如果匹配则返回 Success()
     */
    static Error ValidatePayloadLength(const H2FrameHeader& header, size_t payload_len);

    /**
     * @brief 帧类型转换为字符串
     * @param type 帧类型
     * @return 类型名称
     */
    static std::string FrameTypeToString(H2FrameType type);

    /**
     * @brief 字符串转换为帧类型
     * @param name 类型名称
     * @return 帧类型，如果无效则返回 std::nullopt
     */
    static std::optional<H2FrameType> StringToFrameType(const std::string& name);

    /**
     * @brief 检查流ID是否有效
     * @param stream_id 流标识符
     * @return true 如果流ID有效
     */
    static bool IsValidStreamId(uint32_t stream_id);

    /**
     * @brief 检查帧长度是否有效
     * @param length 帧长度
     * @param type 帧类型
     * @return Error 对象，如果有效则返回 Success()
     */
    static Error ValidateFrameLength(uint32_t length, H2FrameType type);

private:
    // 禁止实例化
    H2FrameParser() = delete;
    ~H2FrameParser() = delete;
};

} // namespace http2
} // namespace tinywebserver