#include "okrapmlib/snapshot.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <ctime>

namespace fs = std::filesystem;

namespace okrapm {

std::string Snapshot::to_string() const {
    std::ostringstream oss;
    std::time_t t = std::chrono::system_clock::to_time_t(timestamp);
    std::tm tm{};
    localtime_r(&t, &tm);

    oss << "#" << std::setw(4) << std::setfill('0') << id << "  "
        << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "  "
        << "(" << objects.size() << " objects)";
    if (!description.empty()) {
        oss << "  [" << description << "]";
    }
    return oss.str();
}

SnapshotManager::SnapshotManager(const std::string& snapshot_dir)
    : snapshot_dir_(snapshot_dir) {
    if (!snapshot_dir_.empty()) {
        std::error_code ec;
        fs::create_directories(snapshot_dir_, ec);
    }
    load();
}

std::string SnapshotManager::snapshot_path(uint64_t id) const {
    return snapshot_dir_ + "/snapshot_" + std::to_string(id) + ".dat";
}

bool SnapshotManager::load() {
    snapshots_.clear();
    std::string index_path = snapshot_dir_ + "/snapshots.idx";
    std::ifstream ifs(index_path);
    if (!ifs.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string id_str, time_str, desc;
        if (std::getline(iss, id_str, '|') &&
            std::getline(iss, time_str, '|')) {
            std::getline(iss, desc);
            try {
                uint64_t id = std::stoull(id_str);
                int64_t time_sec = std::stoll(time_str);

                Snapshot snap;
                snap.id = id;
                snap.timestamp = std::chrono::system_clock::time_point(std::chrono::seconds(time_sec));
                snap.description = desc;
                snap.label = "System #" + std::to_string(id);

                // 读取对应的对象列表
                std::ifstream sifs(snapshot_path(id));
                if (sifs.is_open()) {
                    std::string sline;
                    while (std::getline(sifs, sline)) {
                        if (sline.empty() || sline[0] == '#') continue;
                        auto obj = Object::deserialize(sline);
                        if (obj) snap.objects.push_back(*obj);
                    }
                }
                snapshots_.push_back(snap);
            } catch (...) {}
        }
    }
    return true;
}

bool SnapshotManager::save() const {
    if (!snapshot_dir_.empty()) {
        std::error_code ec;
        fs::create_directories(snapshot_dir_, ec);
    }

    std::string index_path = snapshot_dir_ + "/snapshots.idx";
    std::ofstream ofs(index_path);
    if (!ofs.is_open()) return false;

    ofs << "# Lunar Snapshot Index\n";
    for (const auto& snap : snapshots_) {
        auto time_sec = std::chrono::duration_cast<std::chrono::seconds>(snap.timestamp.time_since_epoch()).count();
        ofs << snap.id << "|" << time_sec << "|" << snap.description << "\n";

        // 保存对应对象的快照
        std::ofstream sofs(snapshot_path(snap.id));
        if (sofs.is_open()) {
            sofs << "# Snapshot #" << snap.id << "\n";
            for (const auto& obj : snap.objects) {
                sofs << obj.serialize() << "\n";
            }
        }
    }
    return true;
}

Snapshot SnapshotManager::create(const SystemStore& store, const std::string& description) {
    Snapshot snap;
    snap.id = snapshots_.empty() ? 1001 : (snapshots_.back().id + 1);
    snap.timestamp = std::chrono::system_clock::now();
    snap.description = description;
    snap.label = "System #" + std::to_string(snap.id);
    snap.objects = store.list_installed();

    snapshots_.push_back(snap);
    save();
    return snap;
}

std::vector<Snapshot> SnapshotManager::list() const {
    return snapshots_;
}

std::optional<Snapshot> SnapshotManager::get(uint64_t id) const {
    for (const auto& snap : snapshots_) {
        if (snap.id == id) return snap;
    }
    return std::nullopt;
}

SnapshotManager::RestorePlan SnapshotManager::restore_plan(uint64_t snapshot_id, const SystemStore& store) const {
    RestorePlan plan;
    auto snap = get(snapshot_id);
    if (!snap) return plan;

    auto current_installed = store.list_installed();

    // 找出在 snapshot 中但当前没有安装，或者版本不同的对象 -> to_install
    for (const auto& target_obj : snap->objects) {
        auto cur_obj = store.find(target_obj.ns(), target_obj.name());
        if (!cur_obj || cur_obj->version() != target_obj.version()) {
            plan.to_install.push_back(target_obj);
        }
    }

    // 找出在当前安装但 snapshot 中没有的对象 -> to_remove
    for (const auto& cur_obj : current_installed) {
        bool in_snapshot = false;
        for (const auto& target_obj : snap->objects) {
            if (cur_obj.ns() == target_obj.ns() && cur_obj.name() == target_obj.name()) {
                in_snapshot = true;
                break;
            }
        }
        if (!in_snapshot) {
            plan.to_remove.push_back(cur_obj);
        }
    }

    return plan;
}

bool SnapshotManager::restore(uint64_t snapshot_id, SystemStore& store) {
    auto plan = restore_plan(snapshot_id, store);
    auto snap = get(snapshot_id);
    if (!snap) return false;

    // 先删除需要移除的
    for (const auto& obj : plan.to_remove) {
        store.remove(obj.ns(), obj.name());
    }

    // 再安装/恢复需要的对象
    for (const auto& obj : plan.to_install) {
        store.install(obj);
    }

    return store.save();
}

bool SnapshotManager::remove(uint64_t id) {
    auto it = std::remove_if(snapshots_.begin(), snapshots_.end(),
                             [id](const Snapshot& s) { return s.id == id; });
    if (it == snapshots_.end()) return false;

    snapshots_.erase(it, snapshots_.end());

    std::error_code ec;
    fs::remove(snapshot_path(id), ec);

    return save();
}

} // namespace okrapm
