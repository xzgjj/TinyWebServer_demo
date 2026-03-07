#pragma once

#include <string>
#include <optional>
#include <chrono>
#include <filesystem>
#include "http_request.h"

namespace tinywebserver {

/**
 * @brief 条件请求处理器
 *
 * 支持 HTTP 条件请求头（If-Modified-Since, If-None-Match），
 * 用于返回 304 Not Modified 响应，减少不必要的数据传输。
 */
class ConditionalRequestHandler {
public:
    /**
     * @brief 文件状态信息
     */
    struct FileStat {
        std::chrono::system_clock::time_point last_modified;  ///< 最后修改时间
        uint64_t file_size;                                   ///< 文件大小（字节）
        std::string etag;                                     ///< 生成的 ETag
    };

    /**
     * @brief 检查是否应返回 304 Not Modified
     * @param request HTTP 请求
     * @param file_path 文件路径
     * @return 如果应返回 304 则为 true，同时返回对应的 ETag
     */
    static std::pair<bool, std::string> ShouldReturn304(
        const HttpRequest& request,
        const std::filesystem::path& file_path);

    /**
     * @brief 检查是否应返回 304 Not Modified（使用预计算的 FileStat）
     * @param request HTTP 请求
     * @param file_stat 文件状态信息
     * @return 如果应返回 304 则为 true
     */
    static bool ShouldReturn304(
        const HttpRequest& request,
        const FileStat& file_stat);

    /**
     * @brief 获取文件状态信息
     * @param file_path 文件路径
     * @return FileStat 对象，如果文件不存在则返回 std::nullopt
     */
    static std::optional<FileStat> GetFileStat(const std::filesystem::path& file_path);

    /**
     * @brief 生成 ETag 字符串
     * @param file_stat 文件状态信息
     * @return ETag 字符串（格式："W/\"<size>-<timestamp>\""）
     */
    static std::string GenerateETag(const FileStat& file_stat);

    /**
     * @brief 生成弱 ETag（不依赖文件内容，只依赖元数据）
     * @param file_stat 文件状态信息
     * @return 弱 ETag 字符串（格式："W/\"<size>-<timestamp>\""）
     */
    static std::string GenerateWeakETag(const FileStat& file_stat);

    /**
     * @brief 检查 If-Modified-Since 条件
     * @param request HTTP 请求
     * @param file_stat 文件状态信息
     * @return true 如果文件在指定时间后未修改
     */
    static bool CheckIfModifiedSince(
        const HttpRequest& request,
        const FileStat& file_stat);

    /**
     * @brief 检查 If-None-Match 条件
     * @param request HTTP 请求
     * @param file_stat 文件状态信息
     * @return true 如果 ETag 不匹配
     */
    static bool CheckIfNoneMatch(
        const HttpRequest& request,
        const FileStat& file_stat);

    /**
     * @brief 解析 HTTP 日期字符串
     * @param date_str HTTP 日期字符串（RFC 7231 格式）
     * @return 时间点，如果解析失败则返回 std::nullopt
     */
    static std::optional<std::chrono::system_clock::time_point> ParseHttpDate(
        const std::string& date_str);

    /**
     * @brief 格式化时间为 HTTP 日期字符串
     * @param time_point 时间点
     * @return HTTP 日期字符串（RFC 7231 格式）
     */
    static std::string FormatHttpDate(
        std::chrono::system_clock::time_point time_point);

private:
    // 禁止实例化
    ConditionalRequestHandler() = delete;
    ~ConditionalRequestHandler() = delete;
};

} // namespace tinywebserver