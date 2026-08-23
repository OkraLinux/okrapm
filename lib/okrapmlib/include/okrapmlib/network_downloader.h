#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace okrapm {

// 下载选项
struct DownloadOptions {
    int timeout_seconds{30};
    int max_retries{3};
    bool verbose{false};
    std::string expected_sha256;
    std::string user_agent{"Lunar-PackageManager/1.0 (OkraLinux)"};
};

// 下载结果
struct DownloadResult {
    bool success{false};
    int status_code{0};
    std::string dest_path;
    std::string checksum;
    size_t bytes_downloaded{0};
    std::string error_message;
};

// 网络下载引擎
class NetworkDownloader {
public:
    // 下载远程文件到本地路径
    static DownloadResult download_file(const std::string& url,
                                        const std::string& dest_path,
                                        const DownloadOptions& options = DownloadOptions());

    // 将远程文本直接下载至内存字符串
    static std::optional<std::string> download_string(const std::string& url,
                                                      const DownloadOptions& options = DownloadOptions());

    // 校验本地文件与期望哈希是否一致
    static bool verify_checksum(const std::string& file_path, const std::string& expected_sha256);

    // 计算文件 SHA256
    static std::string calculate_sha256(const std::string& file_path);
};

} // namespace okrapm
