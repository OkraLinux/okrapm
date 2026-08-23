#include "okrapmlib/version.h"
#include <sstream>
#include <algorithm>

namespace okrapm {

Version::Version(int major, int minor, int patch, std::string pre_release)
    : major_(major), minor_(minor), patch_(patch), pre_release_(std::move(pre_release)) {}

std::optional<Version> Version::parse(const std::string& str) {
    if (str.empty()) return std::nullopt;

    std::string remaining = str;
    std::string pre_release;

    auto dash_pos = str.find('-');
    if (dash_pos != std::string::npos) {
        remaining = str.substr(0, dash_pos);
        pre_release = str.substr(dash_pos + 1);
    }

    std::istringstream iss(remaining);
    std::string token;
    Version v;
    int component = 0;

    while (std::getline(iss, token, '.')) {
        if (component > 2) return std::nullopt;
        try {
            int val = std::stoi(token);
            if (val < 0) return std::nullopt;
            switch (component) {
                case 0: v.major_ = val; break;
                case 1: v.minor_ = val; break;
                case 2: v.patch_ = val; break;
            }
        } catch (...) {
            return std::nullopt;
        }
        component++;
    }

    if (component == 0 || component > 3) return std::nullopt;
    v.pre_release_ = pre_release;
    return v;
}

std::string Version::to_string() const {
    std::string result = std::to_string(major_) + "." +
                         std::to_string(minor_) + "." +
                         std::to_string(patch_);
    if (!pre_release_.empty()) {
        result += "-" + pre_release_;
    }
    return result;
}

int Version::compare_pre_release(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return 1;
    if (b.empty()) return -1;

    std::istringstream iss_a(a), iss_b(b);
    std::string token_a, token_b;

    while (true) {
        bool has_a = static_cast<bool>(std::getline(iss_a, token_a, '.'));
        bool has_b = static_cast<bool>(std::getline(iss_b, token_b, '.'));

        if (!has_a && !has_b) return 0;
        if (!has_a) return -1;
        if (!has_b) return 1;

        bool a_is_num = !token_a.empty() && std::all_of(token_a.begin(), token_a.end(), ::isdigit);
        bool b_is_num = !token_b.empty() && std::all_of(token_b.begin(), token_b.end(), ::isdigit);

        if (a_is_num && b_is_num) {
            long num_a = std::stol(token_a);
            long num_b = std::stol(token_b);
            if (num_a < num_b) return -1;
            if (num_a > num_b) return 1;
        } else if (a_is_num != b_is_num) {
            return a_is_num ? -1 : 1;
        } else {
            if (token_a < token_b) return -1;
            if (token_a > token_b) return 1;
        }
    }
}

bool Version::operator==(const Version& other) const {
    return major_ == other.major_ &&
           minor_ == other.minor_ &&
           patch_ == other.patch_ &&
           pre_release_ == other.pre_release_;
}

bool Version::operator!=(const Version& other) const {
    return !(*this == other);
}

bool Version::operator<(const Version& other) const {
    if (major_ != other.major_) return major_ < other.major_;
    if (minor_ != other.minor_) return minor_ < other.minor_;
    if (patch_ != other.patch_) return patch_ < other.patch_;
    return compare_pre_release(pre_release_, other.pre_release_) < 0;
}

bool Version::operator<=(const Version& other) const {
    return *this == other || *this < other;
}

bool Version::operator>(const Version& other) const {
    return other < *this;
}

bool Version::operator>=(const Version& other) const {
    return other <= *this;
}

} // namespace okrapm
