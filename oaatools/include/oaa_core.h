#pragma once

#include <string>
#include <vector>
#include <optional>
#include <map>

namespace oaa {

struct OaaMeta {
    std::string name;
    std::string ns{"app"};
    std::string version{"1.0.0"};
    std::string description;
    std::string architecture{"x86_64"};
    std::string maintainer;
    size_t installed_size{0};
    std::string checksum;
    std::vector<std::string> dependencies;
    std::vector<std::string> files;

    std::string to_yaml() const;
    static std::optional<OaaMeta> from_yaml_string(const std::string& yaml_str);
    static std::optional<OaaMeta> from_yaml_file(const std::string& path);
};

class OaaCore {
public:
    static bool init_skeleton(const std::string& dir, const OaaMeta& initial_meta);
    static std::optional<std::string> build_package(const std::string& src_dir, const std::string& out_path = "", const std::string& comp = "zstd");
    static std::optional<OaaMeta> inspect_package(const std::string& pkg_path);
    static bool extract_package(const std::string& pkg_path, const std::string& target_dir);
    static bool verify_package(const std::string& pkg_path);
    static std::vector<std::string> list_contents(const std::string& pkg_path);
    static std::string calculate_sha256(const std::string& file_path);
};

} // namespace oaa
