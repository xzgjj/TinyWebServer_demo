//

#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <string>
#include <unordered_map>

enum class HttpMethod { GET, POST, UNKNOWN };
enum class ParseState { REQUEST_LINE, HEADERS, BODY, FINISH };

class HttpRequest {
public:
    HttpRequest() : state_(ParseState::REQUEST_LINE), method_(HttpMethod::UNKNOWN) {}

    // 核心解析接口
    bool Parse(const std::string& data);
    bool IsFinish() const { return state_ == ParseState::FINISH; }

    // 获取解析后的数据
    std::string GetPath() const { return path_; }
    std::string GetHeader(const std::string& key) const;

private:
    // 解析每一行的子逻辑
    bool ParseRequestLine(const std::string& line);
    bool ParseHeader(const std::string& line);

    ParseState state_;
    HttpMethod method_;
    std::string path_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
};

#endif