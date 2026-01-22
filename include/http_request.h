//重构了响应逻辑，支持内存缓冲和零拷贝文件块的混合管理。

#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <string>

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
        is_finished_ = false;
    }

    /**
     * @brief 检查解析是否已完成
     */
    bool IsFinished() const { return is_finished_; }

private:
    std::string path_;
    bool is_finished_;
};

#endif // HTTP_REQUEST_H