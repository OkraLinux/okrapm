#include "okrapmlib/network_downloader.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <unistd.h>

namespace fs = std::filesystem;

namespace okrapm {

static std::string trim(const std::string& str) {
    auto first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    auto last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static std::string exec_cmd(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    return result;
}

std::string NetworkDownloader::calculate_sha256(const std::string& file_path) {
    if (!fs::exists(file_path)) return "";
    std::string cmd = "sha256sum \"" + file_path + "\" 2>/dev/null";
    std::string out = exec_cmd(cmd);
    auto space_pos = out.find(' ');
    if (space_pos != std::string::npos) {
        return out.substr(0, space_pos);
    }
    return trim(out);
}

bool NetworkDownloader::verify_checksum(const std::string& file_path, const std::string& expected_sha256) {
    if (expected_sha256.empty()) return true;
    std::string actual = calculate_sha256(file_path);
    if (actual.empty()) return false;
    return actual == expected_sha256;
}

DownloadResult NetworkDownloader::download_file(const std::string& url,
                                                const std::string& dest_path,
                                                const DownloadOptions& options) {
    DownloadResult res;
    res.dest_path = dest_path;

    if (url.empty() || dest_path.empty()) {
        res.error_message = "URL or destination path is empty";
        return res;
    }

    auto parent = fs::path(dest_path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }

    // 1. 本地 file:// 协议或本地路径
    if (url.rfind("file://", 0) == 0 || url.front() == '/') {
        std::string local_path = (url.rfind("file://", 0) == 0) ? url.substr(7) : url;
        if (!fs::exists(local_path)) {
            res.error_message = "Local file does not exist: " + local_path;
            return res;
        }
        std::error_code ec;
        fs::copy_file(local_path, dest_path, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            res.error_message = "Failed to copy local file: " + ec.message();
            return res;
        }

        if (!options.expected_sha256.empty() && !verify_checksum(dest_path, options.expected_sha256)) {
            res.error_message = "Checksum verification failed";
            fs::remove(dest_path, ec);
            return res;
        }

        res.success = true;
        res.status_code = 200;
        res.checksum = calculate_sha256(dest_path);
        res.bytes_downloaded = fs::file_size(dest_path, ec);
        return res;
    }

    // 2. HTTP / HTTPS 协议
    int retries = 0;
    int max_retries = std::max(1, options.max_retries);

    while (retries < max_retries) {
        std::string temp_dest = dest_path + ".tmp." + std::to_string(getpid());
        std::string cmd = "curl -fsSL --connect-timeout " + std::to_string(options.timeout_seconds) +
                          " -A \"" + options.user_agent + "\"" +
                          " -o \"" + temp_dest + "\" \"" + url + "\" 2>/dev/null";

        int ret = std::system(cmd.c_str());
        if (ret != 0) {
            // fallback to wget
            cmd = "wget -q --timeout=" + std::to_string(options.timeout_seconds) +
                  " --user-agent=\"" + options.user_agent + "\"" +
                  " -O \"" + temp_dest + "\" \"" + url + "\" 2>/dev/null";
            ret = std::system(cmd.c_str());
        }

        if (ret == 0 && fs::exists(temp_dest)) {
            std::error_code ec;
            size_t size = fs::file_size(temp_dest, ec);
            if (size > 0) {
                if (!options.expected_sha256.empty() && !verify_checksum(temp_dest, options.expected_sha256)) {
                    fs::remove(temp_dest, ec);
                    res.error_message = "Checksum mismatch for downloaded artifact from " + url;
                    retries++;
                    continue;
                }

                fs::rename(temp_dest, dest_path, ec);
                if (!ec) {
                    res.success = true;
                    res.status_code = 200;
                    res.checksum = calculate_sha256(dest_path);
                    res.bytes_downloaded = size;
                    return res;
                }
            }
            fs::remove(temp_dest, ec);
        }

        retries++;
    }

    res.error_message = "Failed to download after " + std::to_string(max_retries) + " attempts from: " + url;
    return res;
}

std::optional<std::string> NetworkDownloader::download_string(const std::string& url,
                                                              const DownloadOptions& options) {
    if (url.empty()) return std::nullopt;

    // file:// 或绝对路径
    if (url.rfind("file://", 0) == 0 || url.front() == '/') {
        std::string local_path = (url.rfind("file://", 0) == 0) ? url.substr(7) : url;
        std::ifstream ifs(local_path);
        if (!ifs.is_open()) return std::nullopt;
        return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    std::string cmd = "curl -fsSL --connect-timeout " + std::to_string(options.timeout_seconds) +
                      " -A \"" + options.user_agent + "\" \"" + url + "\" 2>/dev/null";
    std::string content = exec_cmd(cmd);
    if (!content.empty()) {
        return content;
    }

    // fallback to wget
    cmd = "wget -qO- --timeout=" + std::to_string(options.timeout_seconds) +
          " --user-agent=\"" + options.user_agent + "\" \"" + url + "\" 2>/dev/null";
    content = exec_cmd(cmd);
    if (!content.empty()) {
        return content;
    }

    return std::nullopt;
}

} // namespace okrapm
