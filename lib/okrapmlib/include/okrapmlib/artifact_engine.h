#pragma once

#include "object.h"
#include "version.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace okrapm {

// Artifact 元数据
struct ArtifactMetadata {
    std::string ns{"app"};
    std::string name;
    Version version;
    std::string description;
    std::string architecture{"x86_64"};
    std::string maintainer;
    std::vector<std::string> dependencies;
    std::vector<std::string> files;
    std::string checksum;
    size_t download_size{0};
    size_t installed_size{0};

    std::string serialize_yaml() const;
    static std::optional<ArtifactMetadata> parse_yaml(const std::string& content);
    static std::optional<ArtifactMetadata> load_from_file(const std::string& path);

    // 转换为 Lunar Object
    Object to_object() const;
};

// Artifact 打包器
class ArtifactBuilder {
public:
    struct BuildOptions {
        std::string compression{"zstd"}; // "zstd", "xz", "gzip", "none"
        bool verbose{false};
        std::string output_path;          // 若为空则自动生成 <name>-<version>.oaa
    };

    // 构建 Artifact (.oaa / .okra)
    static std::optional<std::string> build(const std::string& source_dir);
    static std::optional<std::string> build(const std::string& source_dir,
                                            const BuildOptions& options);
};

// Artifact 解包与检查器
class ArtifactExtractor {
public:
    // 检查 Artifact 元数据，无需完整解包
    static std::optional<ArtifactMetadata> inspect(const std::string& archive_path);

    // 解包 Artifact 到指定目标目录
    static bool extract(const std::string& archive_path,
                        const std::string& target_dir,
                        bool verbose = false);

    // 执行包内的生命周期钩子脚本 (如 scripts/pre-install 等)
    static int execute_hook(const std::string& script_path,
                            const std::vector<std::string>& args = {});

    // 计算文件 SHA256
    static std::string calculate_sha256(const std::string& file_path);
};

} // namespace okrapm
