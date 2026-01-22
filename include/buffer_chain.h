//

#ifndef BUFFER_CHAIN_H
#define BUFFER_CHAIN_H

#include <deque>
#include <string>
#include <vector>
#include <sys/uio.h> // for iovec
#include <memory>
#include "static_resource_manager.h"

/**
 * @brief 发送缓冲区的基本单元 (异构节点)
 * 可以是内存中的字符串 (Header/Dynamic Content)
 * 也可以是 mmap 的文件块 (Static Content)
 */
struct BufferNode {
    enum Type { STRING, MMAP };
    Type type;
    
    // STRING 类型数据
    std::string str_data;
    
    // MMAP 类型数据
    std::shared_ptr<StaticResource> mmap_res;
    
    // 当前节点已发送的偏移量 (用于断点续传)
    size_t offset = 0;

    // 构造函数：字符串
    explicit BufferNode(std::string str) 
        : type(STRING), str_data(std::move(str)), offset(0) {}

    // 构造函数：mmap 资源
    explicit BufferNode(std::shared_ptr<StaticResource> res) 
        : type(MMAP), mmap_res(std::move(res)), offset(0) {}

    // 获取当前节点剩余可读大小
    size_t LeftSize() const {
        if (type == STRING) {
            return str_data.size() - offset;
        } else if (type == MMAP && mmap_res) {
            return mmap_res->size - offset;
        }
        return 0;
    }

    // 获取当前读取指针
    const char* CurrentPtr() const {
        if (type == STRING) {
            return str_data.data() + offset;
        } else if (type == MMAP && mmap_res) {
            return static_cast<const char*>(mmap_res->addr) + offset;
        }
        return nullptr;
    }
};

/**
 * @brief 链式发送缓冲区
 * 替代 std::string output_buffer_
 * 支持 writev 聚集写，自动管理 mmap 生命周期
 */
class BufferChain {
public:
    // 添加数据到队尾
    void Append(const std::string& data) {
        if (!data.empty()) {
            buffer_queue_.emplace_back(data);
            total_bytes_ += data.size();
        }
    }

    void Append(std::shared_ptr<StaticResource> res) {
        if (res && res->size > 0) {
            buffer_queue_.emplace_back(res);
            total_bytes_ += res->size;
        }
    }

    // 是否为空
    bool IsEmpty() const {
        return buffer_queue_.empty();
    }

    // 获取总剩余字节数
    size_t TotalBytes() const {
        return total_bytes_;
    }

    /**
     * @brief 填充 iovec 数组，准备进行 writev
     * @param iov 输出参数，iovec 数组指针
     * @param max_count 数组最大容量 (通常是 IOV_MAX 或 1024)
     * @return 实际填充的 iovec 数量
     */
    int GetIov(struct iovec* iov, int max_count) {
        if (buffer_queue_.empty()) return 0;

        int count = 0;
        for (const auto& node : buffer_queue_) {
            if (count >= max_count) break;
            
            size_t len = node.LeftSize();
            if (len > 0) {
                iov[count].iov_base = const_cast<char*>(node.CurrentPtr());
                iov[count].iov_len = len;
                count++;
            }
        }
        return count;
    }

    /**
     * @brief 核心逻辑：推进缓冲区状态 (处理已发送字节)
     * @param len 刚刚成功 writev 发送的字节数
     */
    void Advance(size_t len) {
        if (len > total_bytes_) {
            // 防御性编程：理论上不应发生，除非外部逻辑错误
            buffer_queue_.clear();
            total_bytes_ = 0;
            return;
        }

        total_bytes_ -= len;

        while (len > 0 && !buffer_queue_.empty()) {
            BufferNode& front = buffer_queue_.front();
            size_t left = front.LeftSize();

            if (len >= left) {
                // 当前节点已完全发完，移除
                len -= left;
                buffer_queue_.pop_front();
            } else {
                // 当前节点只发了一部分，更新 offset
                front.offset += len;
                len = 0;
            }
        }
    }

    // 清空缓冲区
    void Clear() {
        buffer_queue_.clear();
        total_bytes_ = 0;
    }

private:
    std::deque<BufferNode> buffer_queue_;
    size_t total_bytes_ = 0;
};

#endif // BUFFER_CHAIN_H