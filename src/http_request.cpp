

//有限状态机实现

#include "http_request.h"
#include <sstream>
#include <iostream>

bool HttpRequest::Parse(std::string& buffer) {
    const size_t MAX_LINE_SIZE = 8192; // 限制单行 8KB
    
    while (state_ != ParseState::FINISH && state_ != ParseState::ERROR) {
        size_t pos = buffer.find("\r\n");
        if (pos == std::string::npos) {
            // 安全限制：如果还没收到换行但 buffer 已经超长，判定为恶意攻击
            if (buffer.size() > MAX_LINE_SIZE) state_ = ParseState::ERROR;
            return false; 
        }

        if (pos > MAX_LINE_SIZE) {
            state_ = ParseState::ERROR;
            return false;
        }

        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 2); 

        if (state_ == ParseState::REQUEST_LINE) {
            if (!ParseRequestLine(line)) state_ = ParseState::ERROR;
            else state_ = ParseState::HEADERS;
        } 
        else if (state_ == ParseState::HEADERS) {
            if (line.empty()) {
                state_ = ParseState::FINISH; 
            } else {
                if (!ParseHeader(line)) state_ = ParseState::ERROR;
            }
        }
    }
    return state_ == ParseState::FINISH;
}

bool HttpRequest::ParseRequestLine(const std::string& line) {
    std::stringstream ss(line);
    // 增加对非法 Method 的检查
    if (!(ss >> method_ >> path_ >> version_)) return false;
    if (method_ != "GET" && method_ != "POST") return false; // 简单限制
    return true;
}

bool HttpRequest::ParseHeader(const std::string& line) {
    size_t pos = line.find(": ");
    if (pos != std::string::npos) {
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 2);
        headers_[key] = val;
    }
    return true;
}

void HttpRequest::Reset() {
    state_ = ParseState::REQUEST_LINE;
    method_ = path_ = version_ = body_ = "";
    headers_.clear();
}