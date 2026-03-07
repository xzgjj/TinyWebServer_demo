//重构了响应逻辑，支持内存缓冲和零拷贝文件块的混合管理。

#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <string>
#include <unordered_map>

namespace tinywebserver {

/**
 * @brief HTTP 请求解析类
 * 负责从 Buffer 中提取请求行，主要获取资源路径
 */
class HttpRequest {
public:
    HttpRequest() { Reset(); }
    ~HttpRequest() = default;

    /**
     * @brief 基础 HTTP 解析
     * @param buffer 接收缓冲区
     * @return true 表示解析完成且合法，false 表示数据不足或格式非法
     */
    bool Parse(std::string& buffer);

    /**
     * @brief 获取请求的资源路径
     */
    std::string GetPath() const { return path_; }

    /**
     * @brief 重置解析器状态（用于连接复用）
     */
    void Reset() {
        path_ = "";
        method_ = "";
        version_ = "";
        headers_.clear();
        is_finished_ = false;
    }

    /**
     * @brief 检查解析是否已完成
     */
    bool IsFinished() const { return is_finished_; }

    // ===== 新增接口 =====

    /**
     * @brief 获取 HTTP 方法 (GET, POST, etc.)
     */
    std::string GetMethod() const { return method_; }

    /**
     * @brief 获取 HTTP 版本 (HTTP/1.0, HTTP/1.1)
     */
    std::string GetVersion() const { return version_; }

    /**
     * @brief 获取请求头部映射表
     */
    const std::unordered_map<std::string, std::string>& GetHeaders() const { return headers_; }

    /**
     * @brief 获取指定头部值
     * @param key 头部字段名
     * @return 头部值，如果不存在则返回空字符串
     */
    std::string GetHeader(const std::string& key) const {
        auto it = headers_.find(key);
        return it != headers_.end() ? it->second : "";
    }

    /**
     * @brief 检查请求是否包含 Content-Length 头部
     * @return 内容长度，如果不含该头部则返回 -1
     */
    int64_t GetContentLength() const;

    /**
     * @brief 检查是否为 Keep-Alive 连接
     * @return true 表示应保持连接
     */
    bool IsKeepAlive() const;

private:
    std::string path_;
    std::string method_;      // 新增：HTTP 方法
    std::string version_;     // 新增：HTTP 版本
    std::unordered_map<std::string, std::string> headers_; // 新增：请求头部
    bool is_finished_;

    // 内部解析辅助方法
    bool ParseRequestLine(const std::string& line);
    bool ParseHeaderLine(const std::string& line);
};

} // namespace tinywebserver

// 向后兼容：将 tinywebserver::HttpRequest 引入全局命名空间
using tinywebserver::HttpRequest;

#endif // HTTP_REQUEST_H