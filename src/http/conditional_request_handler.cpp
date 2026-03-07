#include "http/conditional_request_handler.h"
#include "Logger.h"
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstring>

namespace tinywebserver {

namespace fs = std::filesystem;

std::pair<bool, std::string> ConditionalRequestHandler::ShouldReturn304(
    const HttpRequest& request,
    const fs::path& file_path) {

    auto file_stat = GetFileStat(file_path);
    if (!file_stat) {
        // 文件不存在，不应返回 304
        return {false, ""};
    }

    bool should_return_304 = ShouldReturn304(request, *file_stat);
    return {should_return_304, file_stat->etag};
}

bool ConditionalRequestHandler::ShouldReturn304(
    const HttpRequest& request,
    const FileStat& file_stat) {

    // 仅对 GET 和 HEAD 方法应用条件请求
    std::string method = request.GetMethod();
    if (method != "GET" && method != "HEAD") {
        return false;
    }

    // 检查 If-None-Match（优先于 If-Modified-Since）
    if (CheckIfNoneMatch(request, file_stat)) {
        LOG_DEBUG("Conditional request: If-None-Match matched, returning 304");
        return true;
    }

    // 检查 If-Modified-Since
    if (CheckIfModifiedSince(request, file_stat)) {
        LOG_DEBUG("Conditional request: If-Modified-Since matched, returning 304");
        return true;
    }

    return false;
}

std::optional<ConditionalRequestHandler::FileStat> ConditionalRequestHandler::GetFileStat(
    const fs::path& file_path) {

    try {
        if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) {
            return std::nullopt;
        }

        auto ftime = fs::last_write_time(file_path);
        auto file_size = fs::file_size(file_path);

        // 将 filesystem::file_time_type 转换为 system_clock::time_point
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());

        FileStat stat{
            .last_modified = sctp,
            .file_size = static_cast<uint64_t>(file_size),
            .etag = GenerateWeakETag({sctp, static_cast<uint64_t>(file_size), ""})
        };

        return stat;
    } catch (const fs::filesystem_error& e) {
        LOG_ERROR("Failed to get file stat for %s: %s",
                  file_path.string().c_str(), e.what());
        return std::nullopt;
    }
}

std::string ConditionalRequestHandler::GenerateETag(const FileStat& file_stat) {
    // 强 ETag：基于文件内容和元数据（简单实现使用弱 ETag）
    return GenerateWeakETag(file_stat);
}

std::string ConditionalRequestHandler::GenerateWeakETag(const FileStat& file_stat) {
    // 弱 ETag：格式 W/"<size>-<timestamp>"
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        file_stat.last_modified.time_since_epoch()).count();

    std::ostringstream oss;
    oss << "W/\"" << file_stat.file_size << "-" << timestamp << "\"";
    return oss.str();
}

bool ConditionalRequestHandler::CheckIfModifiedSince(
    const HttpRequest& request,
    const FileStat& file_stat) {

    std::string if_modified_since = request.GetHeader("if-modified-since");
    if (if_modified_since.empty()) {
        return false;
    }

    auto since_time = ParseHttpDate(if_modified_since);
    if (!since_time) {
        LOG_WARN("Invalid If-Modified-Since header: %s", if_modified_since.c_str());
        return false;
    }

    // 如果文件在 If-Modified-Since 时间之后没有修改，返回 304
    // 注意：比较时需要忽略秒以下的部分（HTTP 日期精度为秒）
    auto file_time_sec = std::chrono::time_point_cast<std::chrono::seconds>(
        file_stat.last_modified);
    auto since_time_sec = std::chrono::time_point_cast<std::chrono::seconds>(*since_time);

    LOG_DEBUG("If-Modified-Since check: file_time=%ld, since_time=%ld",
              file_time_sec.time_since_epoch().count(),
              since_time_sec.time_since_epoch().count());

    return file_time_sec <= since_time_sec;
}

bool ConditionalRequestHandler::CheckIfNoneMatch(
    const HttpRequest& request,
    const FileStat& file_stat) {

    std::string if_none_match = request.GetHeader("if-none-match");
    if (if_none_match.empty()) {
        return false;
    }

    // 简单实现：检查是否匹配当前文件的 ETag
    // 注意：If-None-Match 可以包含多个 ETag（逗号分隔）或 "*"
    std::string current_etag = file_stat.etag.empty() ?
        GenerateWeakETag(file_stat) : file_stat.etag;

    // 处理 "*"（匹配任何现有资源）
    if (if_none_match == "*") {
        return true;
    }

    // 简单检查：是否包含当前 ETag
    // 实际应解析逗号分隔列表并处理弱 ETag 前缀
    if (if_none_match.find(current_etag) != std::string::npos) {
        return true;
    }

    // 检查弱 ETag 变体（去掉 "W/" 前缀）
    if (current_etag.size() > 3 && current_etag.compare(0, 2, "W/") == 0) {
        std::string weak_etag = current_etag.substr(2);
        if (if_none_match.find(weak_etag) != std::string::npos) {
            return true;
        }
    }

    return false;
}

std::optional<std::chrono::system_clock::time_point> ConditionalRequestHandler::ParseHttpDate(
    const std::string& date_str) {

    // 支持 RFC 7231 定义的三种格式：
    // 1. IMF-fixdate: Sun, 06 Nov 1994 08:49:37 GMT
    // 2. RFC 850-date: Sunday, 06-Nov-94 08:49:37 GMT
    // 3. ANSI C's asctime(): Sun Nov  6 08:49:37 1994

    std::tm tm = {};
    std::istringstream iss(date_str);

    // 尝试 IMF-fixdate 格式
    iss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    if (!iss.fail()) {
        // 转换成功
        auto tt = std::mktime(&tm);
        if (tt == -1) {
            return std::nullopt;
        }
        // 假设时间为 UTC（GMT）
        return std::chrono::system_clock::from_time_t(tt);
    }

    // 重置流状态
    iss.clear();
    iss.str(date_str);

    // 尝试 RFC 850 格式
    tm = {};
    iss >> std::get_time(&tm, "%A, %d-%b-%y %H:%M:%S GMT");
    if (!iss.fail()) {
        auto tt = std::mktime(&tm);
        if (tt == -1) {
            return std::nullopt;
        }
        // 年份处理：two-digit year 可能表示 1900-1999 或 2000-2099
        // 简单处理：如果年份小于 70，假设为 2000+
        if (tm.tm_year < 70) {
            tm.tm_year += 100; // tm_year 是自 1900 的年数
            tt = std::mktime(&tm);
        }
        return std::chrono::system_clock::from_time_t(tt);
    }

    // 重置流状态
    iss.clear();
    iss.str(date_str);

    // 尝试 asctime 格式
    tm = {};
    iss >> std::get_time(&tm, "%a %b %d %H:%M:%S %Y");
    if (!iss.fail()) {
        auto tt = std::mktime(&tm);
        if (tt == -1) {
            return std::nullopt;
        }
        return std::chrono::system_clock::from_time_t(tt);
    }

    LOG_WARN("Failed to parse HTTP date: %s", date_str.c_str());
    return std::nullopt;
}

std::string ConditionalRequestHandler::FormatHttpDate(
    std::chrono::system_clock::time_point time_point) {

    auto tt = std::chrono::system_clock::to_time_t(time_point);
    std::tm tm;

    // 使用 gmtime 获取 UTC 时间
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif

    char buffer[64];
    // IMF-fixdate 格式
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm);

    return std::string(buffer);
}

} // namespace tinywebserver