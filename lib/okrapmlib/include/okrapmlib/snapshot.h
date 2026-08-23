#pragma once

#include "system_store.h"
#include "object.h"
#include "transaction.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace okrapm {

// Snapshot: 系统快照
// 滚动发行版中保存系统状态的安全边界
struct Snapshot {
    uint64_t id{0};
    std::string label;       // 如 "2026-08-23"
    std::vector<Object> objects;  // 快照时的已安装对象
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};
    std::string description;  // 快照描述

    std::string to_string() const;
};

// SnapshotManager: 快照管理器
// 管理系统快照的创建、列出、恢复
class SnapshotManager {
public:
    explicit SnapshotManager(const std::string& snapshot_dir);

    // 创建快照
    Snapshot create(const SystemStore& store, const std::string& description = "");

    // 列出所有快照
    std::vector<Snapshot> list() const;

    // 获取特定快照
    std::optional<Snapshot> get(uint64_t id) const;

    // 恢复到快照 (返回需要安装/删除的对象)
    struct RestorePlan {
        std::vector<Object> to_install;  // 需要安装的对象
        std::vector<Object> to_remove;   // 需要删除的对象
    };
    RestorePlan restore_plan(uint64_t snapshot_id, const SystemStore& store) const;

    // 执行恢复
    bool restore(uint64_t snapshot_id, SystemStore& store);

    // 删除快照
    bool remove(uint64_t id);

    // 持久化
    bool load();
    bool save() const;

private:
    std::string snapshot_dir_;
    std::vector<Snapshot> snapshots_;

    std::string snapshot_path(uint64_t id) const;
};

} // namespace okrapm
