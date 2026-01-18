#include "http_parser.h"

#include <iostream>
#include <sstream>

HttpParser::HttpParser()
    : state_(ParseState::kRequestLine)
{
}

bool HttpParser::Parse(const std::string& data)
{
    buffer_.append(data);

    while (state_ != ParseState::kDone &&
           state_ != ParseState::kError)
    {
        auto line_opt = GetLine();
        if (!line_opt.has_value())
        {
            return false;  // 行未完整，等待更多数据
        }

        const std::string& line = line_opt.value();

        if (state_ == ParseState::kRequestLine)
        {
            if (!ParseRequestLine(line))
            {
                state_ = ParseState::kError;
            }
        }
        else if (state_ == ParseState::kHeaders)
        {
            if (line.empty())
            {
                state_ = ParseState::kDone;
                return true;
            }

            if (!ParseHeaderLine(line))
            {
                state_ = ParseState::kError;
            }
        }
    }

    return state_ == ParseState::kDone;
}

bool HttpParser::ParseRequestLine(const std::string& line)
{
    std::istringstream iss(line);
    if (!(iss >> method_ >> url_ >> version_))
    {
        return false;
    }

    std::cout << "Method: " << method_
              << " URL: " << url_
              << " Version: " << version_ << '\n';

    state_ = ParseState::kHeaders;
    return true;
}

bool HttpParser::ParseHeaderLine(const std::string& line)
{
    const auto pos = line.find(':');
    if (pos == std::string::npos)
    {
        return false;
    }

    const std::string key = line.substr(0, pos);
    const std::string value = line.substr(pos + 1);

    headers_[key] = value;

    std::cout << "Header: [" << key << "] = "
              << value << '\n';

    return true;
}

std::optional<std::string> HttpParser::GetLine()
{
    const auto pos = buffer_.find("\r\n");
    if (pos == std::string::npos)
    {
        return std::nullopt;
    }

    std::string line = buffer_.substr(0, pos);
    buffer_.erase(0, pos + 2);
    return line;
}

bool HttpParser::IsDone() const
{
    return state_ == ParseState::kDone;
}
