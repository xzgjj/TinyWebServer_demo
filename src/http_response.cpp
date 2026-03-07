//

#include "http_response.h"
#include "Logger.h"
#include "http/conditional_request_handler.h"
#include <sstream>

namespace fs = std::filesystem;

const std::unordered_map<std::string, std::string> HttpResponse::SUFFIX_TYPE = {
    {".html", "text/html"},
    {".xml", "text/xml"},
    {".xhtml", "application/xhtml+xml"},
    {".txt", "text/plain"},
    {".rtf", "application/rtf"},
    {".pdf", "application/pdf"},
    {".word", "application/msword"},
    {".png", "image/png"},
    {".gif", "image/gif"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".au", "audio/basic"},
    {".mpeg", "video/mpeg"},
    {".mpg", "video/mpeg"},
    {".avi", "video/x-msvideo"},
    {".gz", "application/x-gzip"},
    {".tar", "application/x-tar"},
    {".css", "text/css"},
    {".js", "text/javascript"},
};

const std::unordered_map<int, std::string> HttpResponse::CODE_STATUS = {
    {200, "OK"},
    {400, "Bad Request"},
    {403, "Forbidden"},
    {404, "Not Found"},
};

const std::unordered_map<int, std::string> HttpResponse::CODE_PATH = {
    {400, "/400.html"},
    {403, "/403.html"},
    {404, "/404.html"},
};

HttpResponse::HttpResponse()
{
    code_ = -1;
    path_ = src_dir_ = "";
    is_keep_alive_ = false;
    file_body_ = nullptr;
}

HttpResponse::~HttpResponse()
{
}

void HttpResponse::Init(const std::string& src_dir, const std::string& path, bool is_keep_alive,
                         int code, const HttpRequest* request)
{
    code_ = code;
    is_keep_alive_ = is_keep_alive;
    path_ = path;
    src_dir_ = src_dir;
    file_body_ = nullptr;
    body_string_.clear();
    status_line_.clear();
    headers_.clear();

    // 条件请求检查（仅当未指定强制状态码且请求有效时）
    if (request && code_ == -1) {
        // 仅对 GET 和 HEAD 方法检查条件请求
        std::string method = request->GetMethod();
        if (method == "GET" || method == "HEAD") {
            // 构建完整文件路径
            std::string full_path = src_dir + path;
            // 使用 ConditionalRequestHandler 检查是否应返回 304
            auto [should_return_304, etag] = tinywebserver::ConditionalRequestHandler::ShouldReturn304(
                *request, fs::path(full_path));
            if (should_return_304) {
                code_ = 304; // Not Modified
                // 存储 ETag 以便在响应头部中添加
                if (!etag.empty()) {
                    headers_["ETag"] = etag;
                }
            }
        }
    }
}

void HttpResponse::MakeResponse()
{
    // 1. 确定资源路径
    std::string full_path = src_dir_ + path_;
    
    // 2. 检查文件状态
    if (code_ == -1) 
    {
        if (fs::exists(full_path) && fs::is_regular_file(full_path)) 
        {
            code_ = 200;
        } 
        else 
        {
            code_ = 404;
        }
    }

    // 3. 处理错误路径映射
    if (CODE_PATH.count(code_)) 
    {
        path_ = CODE_PATH.at(code_);
        full_path = src_dir_ + path_;
    }

    // 4. 加载资源 (mmap 零拷贝)
    if (code_ == 200)
    {
        file_body_ = StaticResourceManager::GetInstance().GetResource(full_path);
        if (!file_body_)
        {
            code_ = 404; // mmap 失败降级
            ErrorHtml_();
        }
    }
    else if (code_ != 304)
    {
        // 对于非 200/304 状态码，生成错误页面
        ErrorHtml_();
    }
    // 304 Not Modified 不需要消息体

    AddStateLine_();
    AddHeader_();
    AddContent_();
}

void HttpResponse::AddStateLine_()
{
    std::string status = CODE_STATUS.count(code_) ? CODE_STATUS.at(code_) : "Unknown";
    status_line_ = "HTTP/1.1 " + std::to_string(code_) + " " + status + "\r\n";
}

void HttpResponse::AddHeader_() {
    // 确保 header_string_ 被重置，防止重复调用叠加
    header_string_ = status_line_;

    // 304 Not Modified 响应特殊处理
    if (code_ == 304) {
        // 304 响应不应有 Content-Type 和 Content-Length
        // 添加必要的头部：Date, ETag (如果存在), Connection
        // Date 头部（当前时间）
        auto now = std::chrono::system_clock::now();
        std::string date_str = tinywebserver::ConditionalRequestHandler::FormatHttpDate(now);
        header_string_ += "Date: " + date_str + "\r\n";

        // ETag 头部（如果已存储）
        auto it = headers_.find("ETag");
        if (it != headers_.end()) {
            header_string_ += "ETag: " + it->second + "\r\n";
        }

        // Connection 头部
        header_string_ += (is_keep_alive_ ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
        header_string_ += "\r\n";
        return;
    }

    // 正常响应的头部
    header_string_ += "Content-Type: " + GetFileType_() + "\r\n";

    // 获取长度：如果是静态文件则取文件大小，否则取错误页面的 body_string_ 大小
    size_t body_len = GetBodyLen();
    header_string_ += "Content-Length: " + std::to_string(body_len) + "\r\n";

    header_string_ += (is_keep_alive_ ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    header_string_ += "\r\n";
}

// 辅助方法：返回 Header 字符串
std::string HttpResponse::GetHeaderString() const {
    return header_string_;
}

void HttpResponse::AddContent_()
{
    headers_["Content-length"] = std::to_string(GetBodyLen());
}

void HttpResponse::ErrorHtml_()
{
    // 简单的内联错误页面，如果磁盘上没有 404.html 则使用此兜底
    if (body_string_.empty())
    {
        body_string_ = "<html><title>Error</title>";
        body_string_ += "<body bgcolor=\"ffffff\">";
        body_string_ += std::to_string(code_) + " : " + (CODE_STATUS.count(code_) ? CODE_STATUS.at(code_) : "Unknown");
        body_string_ += "<hr><em>TinyWebServer</em></body></html>";
    }
}

std::string HttpResponse::GetFileType_()
{
    size_t idx = path_.find_last_of('.');
    if (idx == std::string::npos) return "text/plain";
    
    std::string suffix = path_.substr(idx);
    if (SUFFIX_TYPE.count(suffix)) return SUFFIX_TYPE.at(suffix);
    
    return "text/plain";
}

size_t HttpResponse::GetBodyLen() const
{
    if (file_body_) return file_body_->size;
    return body_string_.size();
}



std::string HttpResponse::GetStatInfo() const
{
    std::stringstream ss;
    ss << "[Response] Code:" << code_ 
       << " Size:" << GetBodyLen() 
       << " Mode:" << (file_body_ ? "Zero-Copy(mmap)" : "Internal-Buffer");
    return ss.str();
}