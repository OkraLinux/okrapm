#pragma once

#include "object.h"
#include "version.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace okrapm {

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

    Object to_object() const;
};

class ArtifactBuilder {
public:
    struct BuildOptions {
        std::string compression{"zstd"};
        bool verbose{false};
        std::string output_path;
    };

    static std::optional<std::string> build(const std::string& source_dir);
    static std::optional<std::string> build(const std::string& source_dir,
                                            const BuildOptions& options);
};

class ArtifactExtractor {
public:

    static std::optional<ArtifactMetadata> inspect(const std::string& archive_path);

    static bool extract(const std::string& archive_path,
                        const std::string& target_dir,
                        bool verbose = false);

    static int execute_hook(const std::string& script_path,
                            const std::vector<std::string>& args = {});

    static std::string calculate_sha256(const std::string& file_path);
};

}
