//并增加状态查询接口。

#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <string>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include "static_resource_manager.h"

/**
 * @brief HTTP 响应类
 * 负责根据请求结果构建协议头部，并关联静态资源块
 */
class HttpResponse
{
public:
    HttpResponse();
    ~HttpResponse();

    /**
     * @brief 初始化响应参数
     */
    void Init(const std::string& src_dir, const std::string& path, bool is_keep_alive = false, int code = -1);
    
    /**
     * @brief 构建响应报文逻辑
     */
    void MakeResponse();

    // 状态查询接口
    std::string GetHeaderString() const;
    std::shared_ptr<StaticResource> GetFileBody() const { return file_body_; }
    std::string GetBodyString() const { return body_string_; }
    size_t GetBodyLen() const;
    bool HasFileBody() const { return file_body_ != nullptr; }
    int GetCode() const { return code_; }

    /**
     * @brief 获取当前响应的诊断信息
     */
    std::string GetStatInfo() const;

private:
    void AddStateLine_();
    void AddHeader_();
    void AddContent_();
    void ErrorHtml_();
    std::string GetFileType_();

    int code_;
    bool is_keep_alive_;
    std::string path_;
    std::string src_dir_;

    std::string status_line_;
    std::string header_string_;  // 拼接后的所有头部字符串
    std::unordered_map<std::string, std::string> headers_;
    
    std::string body_string_; 
    std::shared_ptr<StaticResource> file_body_; 
    
    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE;
    static const std::unordered_map<int, std::string> CODE_STATUS;
    static const std::unordered_map<int, std::string> CODE_PATH;
};

#endif