#pragma once

#include <string>
#include <optional>
#include <cstdint>

namespace okrapm {

class Version {
public:
    Version() = default;
    Version(int major, int minor, int patch, std::string pre_release = "");

    static std::optional<Version> parse(const std::string& str);

    std::string to_string() const;

    bool is_pre_release() const { return !pre_release_.empty(); }

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

}
