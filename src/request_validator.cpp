#include "request_validator.h"
#include "http_request.h"
#include "Logger.h"

#include <algorithm>
#include <filesystem>
#include <cctype>

namespace tinywebserver {

namespace fs = std::filesystem;

RequestValidator::RequestValidator(
    const std::string& root_dir,
    size_t max_request_size,
    size_t max_headers_size)
    : root_dir_(root_dir),
      max_request_size_(max_request_size),
      max_headers_size_(max_headers_size) {
    // 默认允许的 HTTP 方法
    allowed_methods_ = {"GET", "POST", "HEAD", "OPTIONS"};
}

RequestValidator::ValidationResult RequestValidator::ValidateRequest(const HttpRequest& request) {
    // 验证 HTTP 方法
    if (!IsMethodAllowed(request.GetMethod())) {
        return ValidationResult{
            false,
            Error(WebError::kUnsupportedMethod,
                  "HTTP method not allowed: " + request.GetMethod()),
            ""
        };
    }

    // 验证路径
    auto path_result = ValidatePath(request.GetPath());
    if (!path_result.valid) {
        return path_result;
    }

    // 验证头部
    auto headers_result = ValidateHeaders(request.GetHeaders(), request.GetContentLength());
    if (!headers_result.valid) {
        return headers_result;
    }

    // 所有验证通过
    return ValidationResult{
        true,
        Error::Success(),
        path_result.normalized_path
    };
}

RequestValidator::ValidationResult RequestValidator::ValidatePath(const std::string& path) {
    std::string normalized = NormalizePath(path);
    if (normalized.empty()) {
        return ValidationResult{
            false,
            Error(WebError::kInvalidPath, "Invalid path: " + path),
            ""
        };
    }

    if (!IsPathWithinRoot(normalized)) {
        return ValidationResult{
            false,
            Error(WebError::kInvalidPath, "Path traversal attempt detected: " + path),
            ""
        };
    }

    return ValidationResult{
        true,
        Error::Success(),
        normalized
    };
}

RequestValidator::ValidationResult RequestValidator::ValidateHeaders(
    const std::unordered_map<std::string, std::string>& headers,
    int64_t content_length) {

    // 检查头部大小
    size_t headers_size = CalculateHeadersSize(headers);
    if (headers_size > max_headers_size_) {
        return ValidationResult{
            false,
            Error(WebError::kRequestTooLarge,
                  "HTTP headers too large: " + std::to_string(headers_size) +
                  " > " + std::to_string(max_headers_size_)),
            ""
        };
    }

    // 检查内容长度
    if (content_length > 0) {
        if (static_cast<size_t>(content_length) > max_request_size_) {
            return ValidationResult{
                false,
                Error(WebError::kRequestTooLarge,
                      "Request body too large: " + std::to_string(content_length) +
                      " > " + std::to_string(max_request_size_)),
                ""
            };
        }
    }

    // 检查 Host 头部（HTTP/1.1 要求）
    // 注意：某些客户端可能不发送 Host 头部，我们仅记录警告
    if (headers.find("host") == headers.end()) {
        LOG_WARN("Request missing Host header");
    }

    return ValidationResult{
        true,
        Error::Success(),
        ""
    };
}

bool RequestValidator::IsMethodAllowed(const std::string& method) const {
    return allowed_methods_.find(method) != allowed_methods_.end();
}

std::string RequestValidator::NormalizePath(const std::string& path) const {
    if (path.empty()) {
        return "";
    }

    // 移除查询字符串和片段
    std::string clean_path = path;
    size_t query_pos = clean_path.find('?');
    if (query_pos != std::string::npos) {
        clean_path = clean_path.substr(0, query_pos);
    }
    size_t fragment_pos = clean_path.find('#');
    if (fragment_pos != std::string::npos) {
        clean_path = clean_path.substr(0, fragment_pos);
    }

    // 特殊处理根路径
    if (clean_path.empty() || clean_path == "/") {
        return ".";
    }

    // 词法规范化：移除 "." 和 ".."，不依赖文件系统
    std::vector<std::string> parts;
    std::stringstream ss(clean_path);
    std::string part;

    // 按 '/' 分割路径，忽略连续斜杠
    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") {
            // 忽略空部分和当前目录标记
            continue;
        } else if (part == "..") {
            // 上级目录：如果栈不为空则弹出
            if (!parts.empty()) {
                parts.pop_back();
            } else {
                // 尝试遍历到根目录之外，视为非法路径
                return "";
            }
        } else {
            // 正常路径部分
            parts.push_back(part);
        }
    }

    // 重新组合路径
    if (parts.empty()) {
        return ".";
    }

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            result += "/";
        }
        result += parts[i];
    }

    return result;
}

bool RequestValidator::IsPathWithinRoot(const std::string& normalized_path) const {
    try {
        // 使用词法路径检查，不依赖文件实际存在
        fs::path root_path = fs::path(root_dir_).lexically_normal();
        fs::path request_path = (root_path / normalized_path).lexically_normal();

        // 检查请求路径是否以根目录开头（词法上）
        auto root_it = root_path.begin();
        auto req_it = request_path.begin();

        while (root_it != root_path.end() && req_it != request_path.end()) {
            if (*root_it != *req_it) {
                return false;
            }
            ++root_it;
            ++req_it;
        }

        // 如果根路径还有剩余部分，说明请求路径比根路径短
        if (root_it != root_path.end()) {
            return false;
        }

        return true;
    } catch (const fs::filesystem_error& e) {
        LOG_ERROR("Path safety check failed: %s", e.what());
        return false;
    }
}

size_t RequestValidator::CalculateHeadersSize(
    const std::unordered_map<std::string, std::string>& headers) const {
    size_t total = 0;
    for (const auto& [key, value] : headers) {
        total += key.size() + value.size() + 4; // ": " 和 "\r\n"
    }
    total += 2; // 最后的 "\r\n"
    return total;
}

} // namespace tinywebserver