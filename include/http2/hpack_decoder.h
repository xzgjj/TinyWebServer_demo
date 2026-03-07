#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <list>
#include <optional>
#include "error/error.h"

namespace tinywebserver {
namespace http2 {

/**
 * @brief HPACK 头部表示（RFC 7541）
 */
struct HpackHeader {
    std::string name;
    std::string value;
    bool sensitive = false;  // 是否属于敏感头部（需要保护）
};

/**
 * @brief HPACK 解码器（RFC 7541）
 *
 * 实现 HTTP/2 头部压缩解码功能，支持静态表和动态表。
 */
class HpackDecoder {
public:
    /**
     * @brief 构造函数
     * @param max_dynamic_table_size 动态表最大大小（字节）
     */
    explicit HpackDecoder(uint32_t max_dynamic_table_size = 4096);
    ~HpackDecoder();

    /**
     * @brief 解码 HPACK 编码的头部块
     * @param data 原始字节数据
     * @param len 数据长度
     * @return 解码后的头部列表，如果解码失败则返回 std::nullopt
     */
    std::optional<std::vector<HpackHeader>> Decode(const uint8_t* data, size_t len);

    /**
     * @brief 解码单个 HPACK 编码的头部字段
     * @param data 数据指针（输入/输出，解码后指针会移动）
     * @param remaining 剩余数据长度（输入/输出）
     * @param header 输出头部字段
     * @return Error 对象
     */
    Error DecodeHeaderField(const uint8_t*& data, size_t& remaining, HpackHeader& header);

    /**
     * @brief 更新动态表大小限制
     * @param new_size 新的最大表大小（字节）
     * @return Error 对象
     */
    Error UpdateDynamicTableSize(uint32_t new_size);

    /**
     * @brief 获取当前动态表大小
     */
    uint32_t GetDynamicTableSize() const { return dynamic_table_size_; }

    /**
     * @brief 获取动态表使用量
     */
    uint32_t GetDynamicTableUsage() const { return dynamic_table_usage_; }

    /**
     * @brief 清空动态表
     */
    void ClearDynamicTable();

    /**
     * @brief 重置解码器状态（清空动态表）
     */
    void Reset();

    /**
     * @brief 获取静态表大小
     */
    static constexpr size_t GetStaticTableSize() { return STATIC_TABLE_SIZE; }

    /**
     * @brief 从索引获取静态表项
     * @param index 索引（1-based）
     * @return 头部字段，如果索引无效则返回 std::nullopt
     */
    static std::optional<HpackHeader> GetStaticTableEntry(uint32_t index);

private:
    // HPACK 解码辅助方法
    Error DecodeInteger(const uint8_t*& data, size_t& remaining,
                        uint8_t prefix_bits, uint32_t& result);
    Error DecodeString(const uint8_t*& data, size_t& remaining, std::string& result);

    // 动态表管理
    void AddToDynamicTable(const HpackHeader& header);
    void EvictDynamicTable();
    std::optional<HpackHeader> GetDynamicTableEntry(uint32_t index) const;
    std::optional<HpackHeader> GetTableEntry(uint32_t index) const;

    // 静态表（RFC 7541 Appendix A）
    static const size_t STATIC_TABLE_SIZE = 61;
    static const HpackHeader STATIC_TABLE[];

    struct DynamicTableEntry {
        HpackHeader header;
        uint32_t size;  // 占用空间大小（RFC 7541 Section 4.1）
    };

    std::list<DynamicTableEntry> dynamic_table_;  // 新的条目添加到前端
    uint32_t max_dynamic_table_size_;
    uint32_t dynamic_table_size_;   // 当前动态表大小
    uint32_t dynamic_table_usage_;  // 当前动态表使用量

    // 解码状态
    bool decoding_started_;
};

} // namespace http2
} // namespace tinywebserver