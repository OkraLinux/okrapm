#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace okrapm {
namespace detail {

struct PayloadEdit {
    std::filesystem::path path;
    std::filesystem::path backup;
    bool directory{false};
};

bool verify_sidecar(const std::string& path);
bool package_id(const std::string& text);
bool note_new_directories(const std::filesystem::path& dir, std::vector<PayloadEdit>& edits);
bool place_node(const std::filesystem::path& from, const std::filesystem::path& to);
bool copy_payload(const std::filesystem::path& payload, const std::filesystem::path& root,
    std::vector<PayloadEdit>& edits, const std::filesystem::path& backup_root);
void rollback_payload(std::vector<PayloadEdit>& edits);

}
}
