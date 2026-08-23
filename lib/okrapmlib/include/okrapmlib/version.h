#pragma once

#include <string>
#include <optional>
#include <cstdint>

namespace okrapm {

// 语义化版本，支持 @version 语法
// 格式: major.minor.patch[-pre_release]
// 例如: 14.2, 2.41.3, 6.4.2-beta.1
class Version {
public:
    Version() = default;
    Version(int major, int minor, int patch, std::string pre_release = "");

    // 解析版本字符串，如 "14.2" 或 "2.41.3-beta.1"
    static std::optional<Version> parse(const std::string& str);

    // 转为字符串
    std::string to_string() const;

    // 是否有预发布标签
    bool is_pre_release() const { return !pre_release_.empty(); }

    // 比较运算符
    bool operator==(const Version& other) const;
    bool operator!=(const Version& other) const;
    bool operator<(const Version& other) const;
    bool operator<=(const Version& other) const;
    bool operator>(const Version& other) const;
    bool operator>=(const Version& other) const;

    int major() const { return major_; }
    int minor() const { return minor_; }
    int patch() const { return patch_; }
    const std::string& pre_release() const { return pre_release_; }

private:
    int major_{0};
    int minor_{0};
    int patch_{0};
    std::string pre_release_;

    static int compare_pre_release(const std::string& a, const std::string& b);
};

} // namespace okrapm
