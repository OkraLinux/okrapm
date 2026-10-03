#pragma once

#include "system_store.h"
#include "object.h"
#include "transaction.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace okrapm {

struct Snapshot {
    uint64_t id{0};
    std::string label;
    std::vector<Object> objects;
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};
    std::string description;

    std::string to_string() const;
};

class SnapshotManager {
public:
    explicit SnapshotManager(const std::string& snapshot_dir);

    Snapshot create(const SystemStore& store, const std::string& description = "");

    std::vector<Snapshot> list() const;

    std::optional<Snapshot> get(uint64_t id) const;

    struct RestorePlan {
        std::vector<Object> to_install;
        std::vector<Object> to_remove;
    };
    RestorePlan restore_plan(uint64_t snapshot_id, const SystemStore& store) const;

    bool restore(uint64_t snapshot_id, SystemStore& store);

    bool remove(uint64_t id);

    bool load();
    bool save() const;

private:
    std::string snapshot_dir_;
    std::vector<Snapshot> snapshots_;

    std::string snapshot_path(uint64_t id) const;
};

}
