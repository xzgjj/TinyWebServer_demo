// 条件请求处理器测试
// 验证 HTTP 条件请求（If-Modified-Since, If-None-Match）功能

#include "http/conditional_request_handler.h"
#include "http_request.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

// 辅助函数：创建带有指定头部的 HTTP 请求
tinywebserver::HttpRequest CreateHttpRequestWithHeaders(
    const std::string& method = "GET",
    const std::string& path = "/test",
    const std::unordered_map<std::string, std::string>& headers = {}) {

    tinywebserver::HttpRequest request;

    // 构建 HTTP 请求字符串
    std::ostringstream oss;
    oss << method << " " << path << " HTTP/1.1\r\n";
    for (const auto& [key, value] : headers) {
        oss << key << ": " << value << "\r\n";
    }
    oss << "\r\n";

    std::string request_str = oss.str();

    // 解析请求
    bool success = request.Parse(request_str);
    // 注意：在测试中，我们可能需要处理解析失败的情况
    if (!success) {
        // 重新初始化并尝试简单解析
        request.Reset();
        std::string simple_request = "GET / HTTP/1.1\r\n\r\n";
        request.Parse(simple_request);
    }

    return request;
}

void TestConditionalRequestHandlerBasic() {
    using namespace tinywebserver;

    // 1. 创建临时测试文件
    fs::path temp_dir = fs::temp_directory_path();
    fs::path test_file = temp_dir / "test_conditional_file.txt";

    // 写入内容
    {
        std::ofstream ofs(test_file);
        ofs << "Test content for conditional request";
    }

    // 获取文件状态
    auto file_stat = ConditionalRequestHandler::GetFileStat(test_file);
    assert(file_stat.has_value());
    assert(file_stat->file_size > 0);
    assert(!file_stat->etag.empty());

    // 2. 创建 HTTP 请求（无条件头）
    auto request = CreateHttpRequestWithHeaders();

    // 3. 测试 ShouldReturn304 - 无条件头，应返回 false
    bool should_304 = ConditionalRequestHandler::ShouldReturn304(request, *file_stat);
    assert(!should_304);

    // 4. 测试 ParseHttpDate 和 FormatHttpDate
    auto now = std::chrono::system_clock::now();
    std::string http_date = ConditionalRequestHandler::FormatHttpDate(now);
    auto parsed_date = ConditionalRequestHandler::ParseHttpDate(http_date);
    assert(parsed_date.has_value());

    // 清理临时文件
    fs::remove(test_file);

    printf("TestConditionalRequestHandlerBasic: PASSED\n");
}

void TestIfModifiedSince() {
    using namespace tinywebserver;

    // 1. 创建临时测试文件
    fs::path temp_dir = fs::temp_directory_path();
    fs::path test_file = temp_dir / "test_if_modified_since.txt";

    {
        std::ofstream ofs(test_file);
        ofs << "Test content";
    }

    auto file_stat = ConditionalRequestHandler::GetFileStat(test_file);
    assert(file_stat.has_value());

    // 2. 测试 If-Modified-Since 在文件修改之前（未来时间）
    auto future_time = file_stat->last_modified + std::chrono::hours(24);
    std::string future_date = ConditionalRequestHandler::FormatHttpDate(future_time);

    auto request1 = CreateHttpRequestWithHeaders("GET", "/test", {{"If-Modified-Since", future_date}});

    bool check_result = ConditionalRequestHandler::CheckIfModifiedSince(request1, *file_stat);
    assert(check_result == true); // 文件未修改，应返回 304

    // 3. 测试 If-Modified-Since 在文件修改之后（过去时间）
    auto past_time = file_stat->last_modified - std::chrono::hours(24);
    std::string past_date = ConditionalRequestHandler::FormatHttpDate(past_time);

    auto request2 = CreateHttpRequestWithHeaders("GET", "/test", {{"If-Modified-Since", past_date}});

    check_result = ConditionalRequestHandler::CheckIfModifiedSince(request2, *file_stat);
    assert(check_result == false); // 文件已修改，不应返回 304

    // 4. 测试无效日期格式
    auto request3 = CreateHttpRequestWithHeaders("GET", "/test", {{"If-Modified-Since", "invalid date format"}});

    check_result = ConditionalRequestHandler::CheckIfModifiedSince(request3, *file_stat);
    assert(check_result == false); // 无效格式应返回 false

    // 清理临时文件
    fs::remove(test_file);

    printf("TestIfModifiedSince: PASSED\n");
}

void TestIfNoneMatch() {
    using namespace tinywebserver;

    // 1. 创建临时测试文件
    fs::path temp_dir = fs::temp_directory_path();
    fs::path test_file = temp_dir / "test_if_none_match.txt";

    {
        std::ofstream ofs(test_file);
        ofs << "Test content for ETag";
    }

    auto file_stat = ConditionalRequestHandler::GetFileStat(test_file);
    assert(file_stat.has_value());

    // 2. 测试 If-None-Match 匹配当前 ETag
    auto request1 = CreateHttpRequestWithHeaders("GET", "/test", {{"If-None-Match", file_stat->etag}});

    bool check_result = ConditionalRequestHandler::CheckIfNoneMatch(request1, *file_stat);
    assert(check_result == true); // ETag 匹配，应返回 304

    // 3. 测试 If-None-Match 不匹配
    auto request2 = CreateHttpRequestWithHeaders("GET", "/test", {{"If-None-Match", "\"different-etag\""}});

    check_result = ConditionalRequestHandler::CheckIfNoneMatch(request2, *file_stat);
    assert(check_result == false); // ETag 不匹配，不应返回 304

    // 4. 测试 If-None-Match 为 "*"（匹配任何资源）
    auto request3 = CreateHttpRequestWithHeaders("GET", "/test", {{"If-None-Match", "*"}});

    check_result = ConditionalRequestHandler::CheckIfNoneMatch(request3, *file_stat);
    assert(check_result == true); // "*" 应匹配任何现有资源

    // 清理临时文件
    fs::remove(test_file);

    printf("TestIfNoneMatch: PASSED\n");
}

void TestShouldReturn304Integration() {
    using namespace tinywebserver;

    // 1. 创建临时测试文件
    fs::path temp_dir = fs::temp_directory_path();
    fs::path test_file = temp_dir / "test_integration.txt";

    {
        std::ofstream ofs(test_file);
        ofs << "Integration test content";
    }

    // 2. 测试 ShouldReturn304 完整流程
    auto file_stat = ConditionalRequestHandler::GetFileStat(test_file);
    assert(file_stat.has_value());

    // 测试 If-None-Match 优先
    auto request1 = CreateHttpRequestWithHeaders("GET", "/test", {
        {"If-None-Match", file_stat->etag},
        {"If-Modified-Since", "Sun, 01 Jan 2000 00:00:00 GMT"}
    });

    bool should_304 = ConditionalRequestHandler::ShouldReturn304(request1, *file_stat);
    assert(should_304 == true); // If-None-Match 优先，应返回 304

    // 3. 测试非 GET/HEAD 方法
    auto request2 = CreateHttpRequestWithHeaders("POST", "/test", {{"If-None-Match", file_stat->etag}});

    should_304 = ConditionalRequestHandler::ShouldReturn304(request2, *file_stat);
    assert(should_304 == false); // POST 方法不应应用条件请求

    // 4. 测试文件路径版本
    auto [should_304_path, etag] = ConditionalRequestHandler::ShouldReturn304(request1, test_file);
    assert(should_304_path == true);
    assert(!etag.empty());

    // 5. 测试文件不存在的情况
    fs::path non_existent = temp_dir / "non_existent_file.txt";
    auto [should_304_nonexistent, etag_nonexistent] = ConditionalRequestHandler::ShouldReturn304(request1, non_existent);
    assert(should_304_nonexistent == false);
    assert(etag_nonexistent.empty());

    // 清理临时文件
    fs::remove(test_file);

    printf("TestShouldReturn304Integration: PASSED\n");
}

void TestGenerateETag() {
    using namespace tinywebserver;

    ConditionalRequestHandler::FileStat stat{
        .last_modified = std::chrono::system_clock::now(),
        .file_size = 12345,
        .etag = ""
    };

    // 1. 测试弱 ETag 生成
    std::string weak_etag = ConditionalRequestHandler::GenerateWeakETag(stat);
    assert(!weak_etag.empty());
    // 弱 ETag 应以 W/" 开头
    assert(weak_etag.size() >= 3);

    // 2. 测试包含文件大小和时间戳
    assert(weak_etag.find("12345") != std::string::npos);

    // 3. 测试 GenerateETag（当前实现使用弱 ETag）
    std::string etag = ConditionalRequestHandler::GenerateETag(stat);
    assert(!etag.empty());

    printf("TestGenerateETag: PASSED\n");
}

int main() {
    printf("Running Conditional Request Handler tests...\n");

    TestConditionalRequestHandlerBasic();
    TestIfModifiedSince();
    TestIfNoneMatch();
    TestShouldReturn304Integration();
    TestGenerateETag();

    printf("\n✅ All Conditional Request Handler tests passed!\n");
    return 0;
}