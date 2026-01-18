//

#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <string>
#include <unordered_map>

enum class ParseState { REQUEST_LINE, HEADERS, BODY, FINISH, ERROR };

class HttpRequest {
public:
    HttpRequest() : state_(ParseState::REQUEST_LINE) {}

    // 解析主入口：传入缓冲区，返回是否解析完成
    bool Parse(std::string& buffer); 
    
    bool IsFinish() const { return state_ == ParseState::FINISH; }
    bool IsError() const { return state_ == ParseState::ERROR; }
    void Reset();

    std::string GetPath() const { return path_; }
    std::string GetMethod() const { return method_; }

private:
    bool ParseRequestLine(const std::string& line);
    bool ParseHeader(const std::string& line);

    ParseState state_;
    std::string method_, path_, version_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
};

#endif