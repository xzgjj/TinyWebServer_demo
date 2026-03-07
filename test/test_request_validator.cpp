#include "request_validator.h"
#include "http_request.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace tinywebserver;

void TestPathNormalization() {
    RequestValidator validator("./www");

    // 测试路径规范化
    auto result1 = validator.ValidatePath("/index.html");
    assert(result1.valid);
    assert(result1.normalized_path == "index.html");

    auto result2 = validator.ValidatePath("/");
    assert(result2.valid);
    assert(result2.normalized_path == "."); // 根目录

    auto result3 = validator.ValidatePath("/../etc/passwd");
    assert(!result3.valid); // 路径遍历攻击

    auto result4 = validator.ValidatePath("/a/b/../c");
    assert(result4.valid);
    assert(result4.normalized_path == "a/c");

    auto result5 = validator.ValidatePath("/a/./b");
    assert(result5.valid);
    assert(result5.normalized_path == "a/b");

    std::cout << "✓ TestPathNormalization passed" << std::endl;
}

void TestMethodValidation() {
    RequestValidator validator("./www");

    // 默认允许 GET, POST, HEAD, OPTIONS
    assert(validator.IsMethodAllowed("GET"));
    assert(validator.IsMethodAllowed("POST"));
    assert(validator.IsMethodAllowed("HEAD"));
    assert(validator.IsMethodAllowed("OPTIONS"));
    assert(!validator.IsMethodAllowed("DELETE")); // 不在默认列表
    assert(!validator.IsMethodAllowed("PUT"));

    std::cout << "✓ TestMethodValidation passed" << std::endl;
}

void TestRequestValidation() {
    RequestValidator validator("./www", 1024, 512); // 1KB 请求体，512B 头部

    // 创建模拟请求
    HttpRequest request;
    std::string buffer = "GET /index.html HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Content-Length: 10\r\n"
                         "\r\n";
    bool parse_ok = request.Parse(buffer);
    assert(parse_ok);

    auto result = validator.ValidateRequest(request);
    assert(result.valid);
    assert(result.normalized_path == "index.html");

    // 测试过大请求体
    HttpRequest large_request;
    std::string large_buffer = "POST /upload HTTP/1.1\r\n"
                               "Content-Length: 2048\r\n"
                               "\r\n";
    parse_ok = large_request.Parse(large_buffer);
    assert(parse_ok);
    auto large_result = validator.ValidateRequest(large_request);
    assert(!large_result.valid);
    assert(large_result.error.GetCode() == WebError::kRequestTooLarge);

    std::cout << "✓ TestRequestValidation passed" << std::endl;
}

void TestHeaderValidation() {
    RequestValidator validator("./www", 1024, 100); // 头部限制 100B

    // 创建头部过大的请求
    HttpRequest request;
    std::string buffer = "GET /index.html HTTP/1.1\r\n"
                         "Very-Long-Header-Name: "
                         "This is a very long header value that exceeds the limit"
                         "This is a very long header value that exceeds the limit"
                         "This is a very long header value that exceeds the limit\r\n"
                         "\r\n";
    bool parse_ok = request.Parse(buffer);
    assert(parse_ok);

    auto result = validator.ValidateRequest(request);
    assert(!result.valid);
    assert(result.error.GetCode() == WebError::kRequestTooLarge);

    std::cout << "✓ TestHeaderValidation passed" << std::endl;
}

void TestEdgeCases() {
    RequestValidator validator("./www");

    // 测试1: 非法 Content-Length
    {
        HttpRequest request;
        std::string buffer = "GET /index.html HTTP/1.1\r\n"
                             "Content-Length: invalid\r\n"
                             "\r\n";
        bool parse_ok = request.Parse(buffer);
        assert(parse_ok);
        auto result = validator.ValidateRequest(request);
        // Content-Length 无效，解析时会转为-1，验证器应通过（因为<=0忽略）
        // 但我们的验证器目前只检查正数大小，所以应该通过
        assert(result.valid);
    }

    // 测试2: 重复关键头（多个 Content-Length）
    {
        HttpRequest request;
        std::string buffer = "GET /index.html HTTP/1.1\r\n"
                             "Content-Length: 10\r\n"
                             "Content-Length: 20\r\n"
                             "\r\n";
        bool parse_ok = request.Parse(buffer);
        // HttpRequest 解析时后面的会覆盖前面的，但验证器只看到一个值
        // 这里主要测试解析器行为
        assert(parse_ok);
        auto result = validator.ValidateRequest(request);
        assert(result.valid);
    }

    // 测试3: 畸形请求行（缺少部分）
    // HttpRequest::Parse 会返回 false，验证器不会被调用
    // 所以我们不需要测试

    // 测试4: HTTP/1.0 Keep-Alive 与 HTTP/1.1 默认行为
    {
        // HTTP/1.0 无 Connection: keep-alive
        HttpRequest req1;
        std::string buf1 = "GET /index.html HTTP/1.0\r\n"
                           "\r\n";
        assert(req1.Parse(buf1));
        assert(!req1.IsKeepAlive()); // HTTP/1.0 默认关闭

        // HTTP/1.0 有 Connection: keep-alive
        HttpRequest req2;
        std::string buf2 = "GET /index.html HTTP/1.0\r\n"
                           "Connection: keep-alive\r\n"
                           "\r\n";
        assert(req2.Parse(buf2));
        assert(req2.IsKeepAlive());

        // HTTP/1.1 默认保持
        HttpRequest req3;
        std::string buf3 = "GET /index.html HTTP/1.1\r\n"
                           "\r\n";
        assert(req3.Parse(buf3));
        assert(req3.IsKeepAlive());

        // HTTP/1.1 明确关闭
        HttpRequest req4;
        std::string buf4 = "GET /index.html HTTP/1.1\r\n"
                           "Connection: close\r\n"
                           "\r\n";
        assert(req4.Parse(buf4));
        assert(!req4.IsKeepAlive());
    }

    // 测试5: 超大头部（超过限制）
    {
        RequestValidator small_validator("./www", 1024, 50); // 头部限制50B
        HttpRequest request;
        std::string buffer = "GET /index.html HTTP/1.1\r\n"
                             "Very-Long-Header: xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n"
                             "\r\n";
        bool parse_ok = request.Parse(buffer);
        assert(parse_ok);
        auto result = small_validator.ValidateRequest(request);
        assert(!result.valid);
        assert(result.error.GetCode() == WebError::kRequestTooLarge);
    }

    // 测试6: 路径规范化边缘情况
    {
        // 空路径
        auto result1 = validator.ValidatePath("");
        assert(!result1.valid);

        // 只有查询字符串
        auto result2 = validator.ValidatePath("/?query=value");
        assert(result2.valid);
        assert(result2.normalized_path == ".");

        // 多个斜杠
        auto result3 = validator.ValidatePath("//a//b//c");
        assert(result3.valid);
        assert(result3.normalized_path == "a/b/c");

        // 尝试遍历到根目录之上
        auto result4 = validator.ValidatePath("/../../etc/passwd");
        assert(!result4.valid);
    }

    std::cout << "✓ TestEdgeCases passed" << std::endl;
}

int main() {
    std::cout << "Running RequestValidator tests..." << std::endl;

    try {
        TestPathNormalization();
        TestMethodValidation();
        TestRequestValidation();
        TestHeaderValidation();
        TestEdgeCases();

        std::cout << "\n✅ All RequestValidator tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed: " << e.what() << std::endl;
        return 1;
    }
}