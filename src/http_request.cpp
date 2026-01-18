

//有限状态机实现

#include "http_request.h"
#include <sstream>
#include <iostream>

bool HttpRequest::Parse(std::string& buffer) {
    while (state_ != ParseState::FINISH && state_ != ParseState::ERROR) {
        size_t pos = buffer.find("\r\n");
        if (pos == std::string::npos) {
            // 没有发现完整的行，说明数据还没到齐，等待下一波 read
            return false; 
        }

        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 2); // 移除已读行及回车换行符

        if (state_ == ParseState::REQUEST_LINE) {
            if (!ParseRequestLine(line)) state_ = ParseState::ERROR;
            else state_ = ParseState::HEADERS;
        } 
        else if (state_ == ParseState::HEADERS) {
            if (line.empty()) { // 遇到空行，说明 Header 结束
                state_ = ParseState::FINISH; 
            } else {
                ParseHeader(line);
            }
        }
    }
    return state_ == ParseState::FINISH;
}

bool HttpRequest::ParseRequestLine(const std::string& line) {
    std::stringstream ss(line);
    if (!(ss >> method_ >> path_ >> version_)) return false;
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