#include "payload.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;

namespace okrapm {
namespace detail {

std::string shell_sha256(const std::string& path) {
    std::string cmd = "sha256sum \"" + path + "\" 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    char buffer[128]{};
    std::string out;
    if (fgets(buffer, sizeof(buffer), pipe)) out = buffer;
    pclose(pipe);
    auto pos = out.find_first_of(" \\t");
    return pos == std::string::npos ? out : out.substr(0, pos);
}

bool verify_sidecar(const std::string& path) {
    std::ifstream sidecar(path + ".sha256");
    if (!sidecar) return true;
    std::string expected;
    sidecar >> expected;
    return !expected.empty() && expected == shell_sha256(path);
}

bool package_id(const std::string& text) {
    if (text.empty() || !std::isalnum(static_cast<unsigned char>(text[0]))) return false;
    for (unsigned char ch : text) {
        if (std::isalnum(ch) || ch == '.' || ch == '_' || ch == '+' || ch == '-') continue;
        return false;
    }
    return true;
}

bool note_new_directories(const fs::path& dir, std::vector<PayloadEdit>& edits) {
    fs::path current;
    for (const fs::path& part : dir) {
        if (part.empty() || part == "/" || part == ".") {
            if (current.empty()) current = part;
            continue;
        }
        current /= part;
        std::error_code ec;
        if (fs::exists(fs::symlink_status(current, ec))) continue;
        if (!fs::create_directory(current, ec) || ec) return false;
        PayloadEdit edit;
        edit.path = current;
        edit.directory = true;
        edits.push_back(std::move(edit));
    }
    return true;
}

bool place_node(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    auto status = fs::symlink_status(from, ec);
    if (ec) return false;
    if (fs::is_symlink(status)) {
        fs::create_symlink(fs::read_symlink(from, ec), to, ec);
        return !ec;
    }
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::permissions(to, status.permissions(), ec);
    return true;
}

bool copy_payload(const fs::path& payload, const fs::path& root, std::vector<PayloadEdit>& edits,
    const fs::path& backup_root) {
    std::error_code ec;
    fs::create_directories(backup_root, ec);
    if (ec) return false;
    fs::recursive_directory_iterator it(
        payload, fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const auto& entry = *it;
        auto rel = fs::relative(entry.path(), payload, ec);
        if (ec || rel.empty() || rel.native().find("..") != std::string::npos) return false;
        auto dest = root / rel;
        if (entry.is_directory()) {
            if (!note_new_directories(dest, edits)) return false;
            continue;
        }
        if (!entry.is_symlink() && !entry.is_regular_file()) continue;
        if (!note_new_directories(dest.parent_path(), edits)) return false;
        fs::path backup;
        if (fs::exists(fs::symlink_status(dest, ec))) {
            if (fs::is_directory(fs::symlink_status(dest))) return false;
            backup = backup_root / std::to_string(edits.size());
            if (!place_node(dest, backup)) return false;
            fs::remove(dest, ec);
            if (ec) return false;
        }
        bool wrote = false;
        if (entry.is_symlink()) {
            auto target = fs::read_symlink(entry.path(), ec);
            if (!ec) fs::create_symlink(target, dest, ec);
            wrote = !ec;
        } else {
            wrote = place_node(entry.path(), dest);
        }
        PayloadEdit change;
        change.path = dest;
        change.backup = backup;
        edits.push_back(std::move(change));
        if (!wrote) return false;
    }
    return true;
}

void rollback_payload(std::vector<PayloadEdit>& edits) {
    std::error_code ec;
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        if (it->directory) {
            fs::remove(it->path, ec);
            continue;
        }
        fs::remove(it->path, ec);
        if (!it->backup.empty()) place_node(it->backup, it->path);
        fs::remove(it->backup, ec);
    }
    edits.clear();
}

}
}
