#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace okrapm {

struct DownloadOptions {
    int timeout_seconds{30};
    int max_retries{3};
    bool verbose{false};
    std::string expected_sha256;
    std::string user_agent{"Lunar-PackageManager/1.0 (OkraLinux)"};
};

struct DownloadResult {
    bool success{false};
    int status_code{0};
    std::string dest_path;
    std::string checksum;
    size_t bytes_downloaded{0};
    std::string error_message;
};

class NetworkDownloader {
public:

    static DownloadResult download_file(const std::string& url,
                                        const std::string& dest_path,
                                        const DownloadOptions& options = DownloadOptions());

    static std::optional<std::string> download_string(const std::string& url,
                                                      const DownloadOptions& options = DownloadOptions());

    static bool verify_checksum(const std::string& file_path, const std::string& expected_sha256);

    static std::string calculate_sha256(const std::string& file_path);
};

}
