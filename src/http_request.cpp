#include "http_request.h"
#include "Logger.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace tinywebserver {

bool HttpRequest::Parse(std::string& buffer) {
    // 查找请求头结束标记
    size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false; // 数据不足，等待更多数据
    }

    std::string header_block = buffer.substr(0, header_end);
    std::istringstream iss(header_block);
    std::string line;

    // 解析请求行
    if (!std::getline(iss, line)) {
        return false;
    }
    // 移除可能的回车符
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (!ParseRequestLine(line)) {
        LOG_ERROR("Invalid request line: %s", line.c_str());
        return false;
    }

    // 解析头部
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break; // 头部结束
        }
        if (!ParseHeaderLine(line)) {
            LOG_ERROR("Invalid header line: %s", line.c_str());
            return false;
        }
    }

    is_finished_ = true;
    // 注意：不在这里消耗缓冲区，由调用者负责
    return true;
}

bool HttpRequest::ParseRequestLine(const std::string& line) {
    std::istringstream iss(line);
    if (!(iss >> method_ >> path_ >> version_)) {
        return false;
    }

    // 简单路径归一化：如果路径为空或为"/"，则设置为"/index.html"
    if (path_.empty() || path_ == "/") {
        path_ = "/index.html";
    }

    LOG_DEBUG("Parsed request line: %s %s %s",
              method_.c_str(), path_.c_str(), version_.c_str());
    return true;
}

bool HttpRequest::ParseHeaderLine(const std::string& line) {
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }

    std::string key = line.substr(0, colon_pos);
    std::string value = line.substr(colon_pos + 1);

    // 去除 key 前后的空格
    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);

    // 去除 value 前后的空格
    value.erase(0, value.find_first_not_of(" \t"));
    value.erase(value.find_last_not_of(" \t") + 1);

    // 头部字段名大小写不敏感，统一转为小写存储
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    headers_[key] = value;
    LOG_DEBUG("Parsed header: [%s] = %s", key.c_str(), value.c_str());
    return true;
}

int64_t HttpRequest::GetContentLength() const {
    auto it = headers_.find("content-length");
    if (it == headers_.end()) {
        return -1;
    }
    try {
        return std::stoll(it->second);
    } catch (const std::exception& e) {
        LOG_ERROR("Invalid Content-Length header value: %s", it->second.c_str());
        return -1;
    }
}

bool HttpRequest::IsKeepAlive() const {
    // HTTP/1.1 默认 Keep-Alive，除非显式指定 Connection: close
    // HTTP/1.0 默认关闭，除非显式指定 Connection: keep-alive
    std::string connection = GetHeader("connection");
    if (version_ == "HTTP/1.1") {
        // HTTP/1.1: 默认保持连接，除非明确关闭
        return connection.empty() ||
               (connection != "close" && connection != "Close");
    } else if (version_ == "HTTP/1.0") {
        // HTTP/1.0: 默认关闭，除非明确保持
        return connection == "keep-alive" || connection == "Keep-Alive";
    }
    // 未知版本，默认关闭
    return false;
}

} // namespace tinywebserver