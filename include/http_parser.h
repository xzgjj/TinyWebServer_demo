#pragma once

#include <optional>
#include <string>
#include <unordered_map>

/**
 * @brief HTTP 请求解析器（有限状态机）
 */
class HttpParser
{
public:
    enum class ParseState
    {
        kRequestLine,
        kHeaders,
        kDone,
        kError
    };

    /**
     * @brief 构造函数
     */
    HttpParser();

    /**
     * @brief 向解析器输入新读取的数据
     * @param data 新数据
     * @return true 表示请求解析完成
     */
    bool Parse(const std::string& data);

    /**
     * @brief 是否解析完成
     */
    bool IsDone() const;

private:
    bool ParseRequestLine(const std::string& line);
    bool ParseHeaderLine(const std::string& line);

    std::optional<std::string> GetLine();

private:
    ParseState state_;
    std::string buffer_;

    std::string method_;
    std::string url_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
};
