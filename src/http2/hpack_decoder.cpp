#include "http2/hpack_decoder.h"
#include "Logger.h"
#include <cstring>

namespace tinywebserver {
namespace http2 {

// 静态表定义（RFC 7541 Appendix A）
const HpackHeader HpackDecoder::STATIC_TABLE[] = {
    /* 1 */ {"", ""}, // 索引从1开始，0占位
    /* 2 */ {":authority", ""},
    /* 3 */ {":method", "GET"},
    /* 4 */ {":method", "POST"},
    /* 5 */ {":path", "/"},
    /* 6 */ {":path", "/index.html"},
    /* 7 */ {":scheme", "http"},
    /* 8 */ {":scheme", "https"},
    /* 9 */ {":status", "200"},
    /* 10 */ {":status", "204"},
    /* 11 */ {":status", "206"},
    /* 12 */ {":status", "304"},
    /* 13 */ {":status", "400"},
    /* 14 */ {":status", "404"},
    /* 15 */ {":status", "500"},
    /* 16 */ {"accept-charset", ""},
    /* 17 */ {"accept-encoding", "gzip, deflate"},
    /* 18 */ {"accept-language", ""},
    /* 19 */ {"accept-ranges", ""},
    /* 20 */ {"accept", ""},
    /* 21 */ {"access-control-allow-origin", ""},
    /* 22 */ {"age", ""},
    /* 23 */ {"allow", ""},
    /* 24 */ {"authorization", ""},
    /* 25 */ {"cache-control", ""},
    /* 26 */ {"content-disposition", ""},
    /* 27 */ {"content-encoding", ""},
    /* 28 */ {"content-language", ""},
    /* 29 */ {"content-length", ""},
    /* 30 */ {"content-location", ""},
    /* 31 */ {"content-range", ""},
    /* 32 */ {"content-type", ""},
    /* 33 */ {"cookie", ""},
    /* 34 */ {"date", ""},
    /* 35 */ {"etag", ""},
    /* 36 */ {"expect", ""},
    /* 37 */ {"expires", ""},
    /* 38 */ {"from", ""},
    /* 39 */ {"host", ""},
    /* 40 */ {"if-match", ""},
    /* 41 */ {"if-modified-since", ""},
    /* 42 */ {"if-none-match", ""},
    /* 43 */ {"if-range", ""},
    /* 44 */ {"if-unmodified-since", ""},
    /* 45 */ {"last-modified", ""},
    /* 46 */ {"link", ""},
    /* 47 */ {"location", ""},
    /* 48 */ {"max-forwards", ""},
    /* 49 */ {"proxy-authenticate", ""},
    /* 50 */ {"proxy-authorization", ""},
    /* 51 */ {"range", ""},
    /* 52 */ {"referer", ""},
    /* 53 */ {"refresh", ""},
    /* 54 */ {"retry-after", ""},
    /* 55 */ {"server", ""},
    /* 56 */ {"set-cookie", ""},
    /* 57 */ {"strict-transport-security", ""},
    /* 58 */ {"transfer-encoding", ""},
    /* 59 */ {"user-agent", ""},
    /* 60 */ {"vary", ""},
    /* 61 */ {"via", ""},
    /* 62 */ {"www-authenticate", ""}
};

HpackDecoder::HpackDecoder(uint32_t max_dynamic_table_size)
    : max_dynamic_table_size_(max_dynamic_table_size),
      dynamic_table_size_(0),
      dynamic_table_usage_(0),
      decoding_started_(false) {

    LOG_DEBUG("HPACK decoder created with max dynamic table size: %u", max_dynamic_table_size_);
}

HpackDecoder::~HpackDecoder() {
    ClearDynamicTable();
}

std::optional<std::vector<HpackHeader>> HpackDecoder::Decode(const uint8_t* data, size_t len) {
    std::vector<HpackHeader> headers;
    const uint8_t* current = data;
    size_t remaining = len;

    decoding_started_ = true;

    while (remaining > 0) {
        HpackHeader header;
        Error err = DecodeHeaderField(current, remaining, header);
        if (!err.IsSuccess()) {
            LOG_ERROR("Failed to decode HPACK header field: %s", err.ToString().c_str());
            return std::nullopt;
        }

        headers.push_back(std::move(header));
    }

    return headers;
}

Error HpackDecoder::DecodeHeaderField(const uint8_t*& data, size_t& remaining, HpackHeader& header) {
    if (remaining == 0) {
        return Error(WebError::kParseError, "No data remaining for HPACK decoding");
    }

    uint8_t first_byte = data[0];

    // 检查索引字段表示（RFC 7541 Section 6.1）
    if ((first_byte & 0x80) != 0) {
        // 索引字段（6.1）
        uint32_t index;
        auto err = DecodeInteger(data, remaining, 7, index);
        if (!err.IsSuccess()) {
            return err;
        }

        auto entry = GetTableEntry(index);
        if (!entry) {
            return Error(WebError::kParseError, "Invalid HPACK table index: " + std::to_string(index));
        }

        header = *entry;
        return Error::Success();
    }
    // 检查字面值字段（6.2）和动态表大小更新（6.3）
    else if ((first_byte & 0xC0) == 0x40) {
        // 字面值字段，带索引名称（6.2.1）
        uint32_t name_index;
        auto err = DecodeInteger(data, remaining, 6, name_index);
        if (!err.IsSuccess()) {
            return err;
        }

        auto name_entry = GetTableEntry(name_index);
        if (!name_entry) {
            return Error(WebError::kParseError, "Invalid HPACK name index: " + std::to_string(name_index));
        }

        header.name = name_entry->name;

        // 解码值
        err = DecodeString(data, remaining, header.value);
        if (!err.IsSuccess()) {
            return err;
        }

        // 添加到动态表
        AddToDynamicTable(header);
        return Error::Success();
    }
    else if ((first_byte & 0xF0) == 0x00) {
        // 字面值字段，带新名称（6.2.2）
        // 解码名称
        auto err = DecodeString(data, remaining, header.name);
        if (!err.IsSuccess()) {
            return err;
        }

        // 解码值
        err = DecodeString(data, remaining, header.value);
        if (!err.IsSuccess()) {
            return err;
        }

        // 添加到动态表
        AddToDynamicTable(header);
        return Error::Success();
    }
    else if ((first_byte & 0xE0) == 0x20) {
        // 动态表大小更新（6.3）
        if (!decoding_started_) {
            return Error(WebError::kParseError, "Dynamic table size update at invalid position");
        }

        uint32_t new_size;
        auto err = DecodeInteger(data, remaining, 5, new_size);
        if (!err.IsSuccess()) {
            return err;
        }

        err = UpdateDynamicTableSize(new_size);
        if (!err.IsSuccess()) {
            return err;
        }

        // 递归解码下一个字段
        return DecodeHeaderField(data, remaining, header);
    }
    else {
        // 未知编码
        return Error(WebError::kParseError, "Unknown HPACK encoding: 0x" +
                     std::to_string(static_cast<int>(first_byte)));
    }
}

Error HpackDecoder::DecodeInteger(const uint8_t*& data, size_t& remaining,
                                  uint8_t prefix_bits, uint32_t& result) {
    if (remaining == 0) {
        return Error(WebError::kParseError, "No data for integer decoding");
    }

    uint8_t mask = (1 << prefix_bits) - 1;
    result = data[0] & mask;
    data++;
    remaining--;

    if (result == mask) {
        // 多字节整数
        uint32_t m = 0;
        uint8_t byte;

        do {
            if (remaining == 0) {
                return Error(WebError::kParseError, "Incomplete multi-byte integer");
            }

            byte = data[0];
            data++;
            remaining--;

            result += (byte & 0x7F) << m;
            m += 7;

            if (m > 28) { // 限制防止溢出
                return Error(WebError::kParseError, "Integer too large");
            }
        } while ((byte & 0x80) != 0);
    }

    return Error::Success();
}

Error HpackDecoder::DecodeString(const uint8_t*& data, size_t& remaining, std::string& result) {
    if (remaining == 0) {
        return Error(WebError::kParseError, "No data for string decoding");
    }

    uint8_t first_byte = data[0];
    bool huffman_encoded = (first_byte & 0x80) != 0;
    uint32_t length;

    auto err = DecodeInteger(data, remaining, 7, length);
    if (!err.IsSuccess()) {
        return err;
    }

    if (remaining < length) {
        return Error(WebError::kParseError, "Incomplete string data");
    }

    if (huffman_encoded) {
        // TODO: 实现哈夫曼解码（RFC 7541 Appendix B）
        LOG_WARN("Huffman decoding not implemented, using raw bytes");
        result.assign(reinterpret_cast<const char*>(data), length);
    } else {
        result.assign(reinterpret_cast<const char*>(data), length);
    }

    data += length;
    remaining -= length;

    return Error::Success();
}

Error HpackDecoder::UpdateDynamicTableSize(uint32_t new_size) {
    if (new_size > max_dynamic_table_size_) {
        return Error(WebError::kProtocolError,
                     "Dynamic table size exceeds maximum: " +
                     std::to_string(new_size) + " > " +
                     std::to_string(max_dynamic_table_size_));
    }

    max_dynamic_table_size_ = new_size;
    EvictDynamicTable();

    LOG_DEBUG("Updated dynamic table size to %u", new_size);
    return Error::Success();
}

void HpackDecoder::ClearDynamicTable() {
    dynamic_table_.clear();
    dynamic_table_size_ = 0;
    dynamic_table_usage_ = 0;
    LOG_DEBUG("Cleared dynamic table");
}

void HpackDecoder::Reset() {
    ClearDynamicTable();
    decoding_started_ = false;
}

std::optional<HpackHeader> HpackDecoder::GetStaticTableEntry(uint32_t index) {
    if (index == 0 || index > STATIC_TABLE_SIZE) {
        return std::nullopt;
    }
    return STATIC_TABLE[index];
}

void HpackDecoder::AddToDynamicTable(const HpackHeader& header) {
    // 计算条目大小（RFC 7541 Section 4.1）
    uint32_t entry_size = static_cast<uint32_t>(header.name.size() + header.value.size() + 32);

    // 确保表有足够空间
    if (entry_size > max_dynamic_table_size_) {
        // 条目太大，无法添加到表中
        LOG_DEBUG("HPACK entry too large for dynamic table: %u > %u",
                  entry_size, max_dynamic_table_size_);
        return;
    }

    // 驱逐旧条目直到有足够空间
    while (!dynamic_table_.empty() &&
           dynamic_table_usage_ + entry_size > max_dynamic_table_size_) {
        EvictDynamicTable();
    }

    // 添加新条目到表前端
    DynamicTableEntry entry{header, entry_size};
    dynamic_table_.push_front(entry);
    dynamic_table_usage_ += entry_size;
    dynamic_table_size_++;

    LOG_DEBUG("Added HPACK entry to dynamic table: %s: %s (size=%u, usage=%u/%u)",
              header.name.c_str(), header.value.c_str(),
              entry_size, dynamic_table_usage_, max_dynamic_table_size_);
}

void HpackDecoder::EvictDynamicTable() {
    if (dynamic_table_.empty()) {
        return;
    }

    auto& entry = dynamic_table_.back();
    dynamic_table_usage_ -= entry.size;
    dynamic_table_size_--;
    dynamic_table_.pop_back();

    LOG_DEBUG("Evicted HPACK entry from dynamic table, usage=%u/%u",
              dynamic_table_usage_, max_dynamic_table_size_);
}

std::optional<HpackHeader> HpackDecoder::GetDynamicTableEntry(uint32_t index) const {
    // 动态表索引从1开始，对应表中最新的条目
    if (index == 0 || index > dynamic_table_.size()) {
        return std::nullopt;
    }

    // 动态表：索引1对应表前端（最新条目）
    auto it = dynamic_table_.begin();
    std::advance(it, index - 1);
    return it->header;
}

std::optional<HpackHeader> HpackDecoder::GetTableEntry(uint32_t index) const {
    if (index <= STATIC_TABLE_SIZE) {
        return GetStaticTableEntry(index);
    } else {
        uint32_t dynamic_index = index - STATIC_TABLE_SIZE;
        return GetDynamicTableEntry(dynamic_index);
    }
}

} // namespace http2
} // namespace tinywebserver