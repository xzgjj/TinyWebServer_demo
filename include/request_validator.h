#pragma once

#include <string>
#include <unordered_map>
#include <set>
#include "error/error.h"

namespace tinywebserver {

class HttpRequest;

/**
 * @brief HTTP 请求验证器
 *
 * 提供请求安全性验证，包括路径安全检查、头部验证、请求大小限制等。
 */
class RequestValidator {
public:
    struct ValidationResult {
        bool valid;                     ///< 验证是否通过
        Error error;                    ///< 错误信息（如果验证失败）
        std::string normalized_path;    ///< 规范化后的路径
    };

    /**
     * @brief 构造函数
     * @param root_dir 允许的根目录（用于路径安全检查）
     * @param max_request_size 最大请求体大小（字节）
     * @param max_headers_size 最大头部大小（字节）
     */
    explicit RequestValidator(
        const std::string& root_dir = "./www",
        size_t max_request_size = 64 * 1024,      // 64KB
        size_t max_headers_size = 8 * 1024        // 8KB
    );

    /**
     * @brief 验证 HTTP 请求
     * @param request HTTP 请求对象
     * @return 验证结果
     */
    ValidationResult ValidateRequest(const HttpRequest& request);

    /**
     * @brief 验证请求路径安全性
     * @param path 请求路径
     * @return 验证结果
     */
    ValidationResult ValidatePath(const std::string& path);

    /**
     * @brief 验证请求头部
     * @param headers 请求头部映射表
     * @param content_length 内容长度（如果存在）
     * @return 验证结果
     */
    ValidationResult ValidateHeaders(
        const std::unordered_map<std::string, std::string>& headers,
        int64_t content_length);

    /**
     * @brief 验证 HTTP 方法是否允许
     * @param method HTTP 方法
     * @return true 如果方法允许
     */
    bool IsMethodAllowed(const std::string& method) const;

    /**
     * @brief 设置允许的 HTTP 方法
     * @param methods 方法集合
     */
    void SetAllowedMethods(const std::set<std::string>& methods) {
        allowed_methods_ = methods;
    }

    /**
     * @brief 获取根目录
     */
    const std::string& GetRootDir() const { return root_dir_; }

    /**
     * @brief 设置根目录
     */
    void SetRootDir(const std::string& root_dir) { root_dir_ = root_dir; }

    /**
     * @brief 获取最大请求体大小
     */
    size_t GetMaxRequestSize() const { return max_request_size_; }

    /**
     * @brief 获取最大头部大小
     */
    size_t GetMaxHeadersSize() const { return max_headers_size_; }

private:
    /**
     * @brief 规范化路径，防止目录遍历攻击
     * @param path 原始路径
     * @return 规范化后的路径，如果路径非法则返回空字符串
     */
    std::string NormalizePath(const std::string& path) const;

    /**
     * @brief 检查路径是否在根目录内
     * @param normalized_path 规范化后的路径
     * @return true 如果路径安全
     */
    bool IsPathWithinRoot(const std::string& normalized_path) const;

    /**
     * @brief 计算头部总大小（估算）
     */
    size_t CalculateHeadersSize(
        const std::unordered_map<std::string, std::string>& headers) const;

    std::string root_dir_;
    size_t max_request_size_;
    size_t max_headers_size_;
    std::set<std::string> allowed_methods_;
};

} // namespace tinywebserver