#include "okrapmlib/lunar_core.h"
#include "okrapmlib/artifact_engine.h"
#include "okrapmlib/lifecycle.h"
#include "payload.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

namespace okrapm {

using detail::PayloadEdit;
using detail::copy_payload;
using detail::package_id;
using detail::rollback_payload;
using detail::verify_sidecar;

bool LunarCore::commit_transaction(Transaction& txn) {
    struct AppliedPackage {
        std::vector<PayloadEdit> files;
        fs::path backup_root;
        fs::path program_live;
        fs::path program_previous;
        bool program_replaced{false};
    };
    std::vector<AppliedPackage> applied;
    std::vector<Operation> pending_store;
    txn.advance_state(TransactionState::Verified);
    extensions().trigger_hooks(HookType::PreTransaction, txn);

    txn.advance_state(TransactionState::Committing);

    snapshot_mgr_->create(*system_store_, "Auto snapshot before txn " + std::to_string(txn.id()));

    auto rollback_applied = [](AppliedPackage& item) {
        rollback_payload(item.files);
        std::error_code ec;
        fs::remove_all(item.backup_root, ec);
        if (item.program_live.empty()) return;
        fs::remove_all(item.program_live, ec);
        if (item.program_replaced) fs::rename(item.program_previous, item.program_live, ec);
    };
    auto fail_transaction = [&](const std::string& message) {
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) rollback_applied(*it);
        txn.advance_state(TransactionState::Failed, message);
        txn.advance_state(TransactionState::RolledBack);
        return false;
    };

    auto install_root_for = [&]() {
        const char* configured_root = std::getenv("LUNAR_INSTALL_ROOT");
        fs::path install_root = configured_root ? configured_root : "/";
        if (!configured_root && fs::status(install_root).permissions() != fs::perms::unknown &&
            (fs::status(install_root).permissions() & fs::perms::owner_write) == fs::perms::none) {
            install_root = data_dir_ + "/rootfs";
        }
        return install_root;
    };

    auto read_architecture = [](const fs::path& staging) {
        std::ifstream input(staging / "meta.yaml");
        std::string line;
        const std::string key = "architecture:";
        while (std::getline(input, line)) {
            if (line.compare(0, key.size(), key) != 0) continue;
            std::string value = line.substr(key.size());
            auto start = value.find_first_not_of(" \t");
            if (start == std::string::npos) break;
            value = value.substr(start);
            if (!value.empty() && value.front() == '"') value.erase(value.begin());
            if (!value.empty() && value.back() == '"') value.pop_back();
            if (!value.empty()) return value;
        }
        return lifecycle_runner() ? lifecycle_runner()->host_architecture() : std::string("unknown");
    };
    auto scene_for = [&](const Operation& op, const fs::path& staging, const std::string& entry) {
        LifecycleScene scene;
        scene.ns = op.target().ns();
        scene.name = op.target().name();
        scene.version = op.target().version().to_string();
        scene.new_version = scene.version;
        if (auto current = system_store_->find(op.target().ns(), op.target().name())) {
            scene.old_version = current->version().to_string();
        }
        scene.architecture = read_architecture(staging);
        scene.reason = entry;
        return scene;
    };
    auto keep_program = [&](const fs::path& staging, const Object& target, AppliedPackage& item) {
        auto runner = lifecycle_runner();
        if (!runner) return false;
        if (!package_id(target.ns()) || !package_id(target.name())) return false;
        fs::path live = fs::path(data_dir_) / "pkg-scripts" / target.ns() / target.name();
        fs::path incoming = live.parent_path() / (target.name() + ".incoming");
        fs::path previous = live.parent_path() / (target.name() + ".previous");
        std::error_code ec;
        fs::remove_all(incoming, ec);
        ec.clear();
        fs::create_directories(incoming, ec);
        if (ec) return false;
        if (!runner->collect_program(staging.string(), incoming.string())) {
            fs::remove_all(incoming, ec);
            return false;
        }
        bool replaced = false;
        if (fs::exists(live)) {
            fs::remove_all(previous, ec);
            ec.clear();
            fs::rename(live, previous, ec);
            if (ec) {
                fs::remove_all(incoming, ec);
                return false;
            }
            replaced = true;
        }
        fs::rename(incoming, live, ec);
        if (ec) {
            std::error_code undo;
            if (replaced) fs::rename(previous, live, undo);
            fs::remove_all(incoming, undo);
            return false;
        }
        item.program_live = live;
        item.program_previous = previous;
        item.program_replaced = replaced;
        return true;
    };

    auto apply_package = [&](const fs::path& staging, AppliedPackage& item, const Operation& op,
        bool upgrade) -> const char* {
        if (!package_id(op.target().ns()) || !package_id(op.target().name())) {
            return "invalid package identifier";
        }
        fs::path payload = fs::exists(staging / "files") ? staging / "files" : staging / "rootfs";
        bool has_payload = fs::is_directory(payload);
        auto runner = lifecycle_runner();
        bool has_opsis = runner && runner->has_program(staging.string());
        if (!has_payload && !has_opsis) return "package has no payload or install.opsis";
        if (has_payload && !copy_payload(payload, install_root_for(), item.files, item.backup_root)) {
            return "Artifact installation failed or file conflict detected";
        }
        std::string entry = upgrade ? "UPDATE" : "INSTALL";
        if (runner) {
            LifecycleScene scene = scene_for(op, staging, entry);
            if (runner->run(staging.string(), entry, install_root_for().string(), scene) != 0) {
                return entry == "UPDATE" ? "OPSIS update script failed" : "OPSIS install script failed";
            }
        }
        if (!keep_program(staging, op.target(), item)) return "failed to save package program";
        return nullptr;
    };

    for (const auto& op : txn.operations()) {
        switch (op.type()) {
            case OperationType::Install:
            case OperationType::Update:
            case OperationType::Upgrade:
            case OperationType::Sync: {
                if (!package_id(op.target().ns()) || !package_id(op.target().name())) {
                    return fail_transaction("invalid package identifier");
                }
                extensions().trigger_hooks(HookType::PreInstall, txn);
                if (op.target().repository().rfind("local:", 0) == 0) {
                    std::string local_path = op.target().repository().substr(6);
                    if (!fs::exists(local_path) || !verify_sidecar(local_path)) {
                        return fail_transaction("Local artifact SHA256 verification failed");
                    }
                    auto staging = fs::temp_directory_path() / ("lunar-install-" + std::to_string(txn.id()));
                    AppliedPackage item;
                    item.backup_root = fs::temp_directory_path()
                        / ("lunar-backup-" + std::to_string(txn.id()) + "-" + op.target().name());
                    bool extracted = ArtifactExtractor::extract(local_path, staging.string());
                    bool upgrade = op.type() == OperationType::Update || op.type() == OperationType::Upgrade
                        || system_store_->is_installed(op.target().ns(), op.target().name());
                    const char* package_error = extracted ? apply_package(staging, item, op, upgrade)
                                                          : "Local artifact installation failed";
                    if (package_error) {
                        rollback_applied(item);
                        fs::remove_all(staging);
                        return fail_transaction(package_error);
                    }
                    fs::remove_all(staging);
                    applied.push_back(std::move(item));
                } else if (op.target().repository() == "local-artifact") {

                } else {
                    auto repo = repo_mgr_->get_repository_for_object(op.target());

                    if (repo && repo->type_name() == "remote") {
                        auto art_path = repo->fetch_artifact(op.target());
                        if (!art_path || !fs::exists(*art_path)) {
                            return fail_transaction("Failed to fetch artifact for " + op.target().full_name());
                        }
                        auto staging = fs::temp_directory_path()
                            / ("lunar-install-" + std::to_string(txn.id()) + "-" + op.target().name());
                        AppliedPackage item;
                        item.backup_root = fs::temp_directory_path()
                            / ("lunar-backup-" + std::to_string(txn.id()) + "-" + op.target().name());
                        bool extracted = ArtifactExtractor::extract(*art_path, staging.string());
                        bool upgrade = op.type() == OperationType::Update || op.type() == OperationType::Upgrade
                            || system_store_->is_installed(op.target().ns(), op.target().name());
                        const char* package_error = extracted ? apply_package(staging, item, op, upgrade)
                                                              : "Artifact installation failed";
                        if (package_error) {
                            rollback_applied(item);
                            fs::remove_all(staging);
                            return fail_transaction(package_error);
                        }
                        fs::remove_all(staging);
                        applied.push_back(std::move(item));
                    }
                }
                pending_store.push_back(op);
                break;
            }
            case OperationType::Remove:
            case OperationType::Purge: {
                if (!package_id(op.target().ns()) || !package_id(op.target().name())) {
                    return fail_transaction("invalid package identifier");
                }
                fs::path saved = fs::path(data_dir_) / "pkg-scripts" / op.target().ns() / op.target().name();
                auto runner = lifecycle_runner();
                if (runner) {
                    LifecycleScene scene;
                    scene.ns = op.target().ns();
                    scene.name = op.target().name();
                    if (auto current = system_store_->find(op.target().ns(), op.target().name())) {
                        scene.version = current->version().to_string();
                        scene.old_version = scene.version;
                    }
                    scene.reason = "REMOVE";
                    if (runner->run(saved.string(), "REMOVE", install_root_for().string(), scene) != 0) {
                        return fail_transaction("OPSIS remove script failed");
                    }
                }
                pending_store.push_back(op);
                break;
            }
        }
    }

    for (const auto& op : pending_store) {
        switch (op.type()) {
            case OperationType::Install:
            case OperationType::Update:
            case OperationType::Upgrade:
            case OperationType::Sync:
                system_store_->install(op.target());
                extensions().trigger_hooks(HookType::PostInstall, txn);
                break;
            case OperationType::Remove:
            case OperationType::Purge:
                extensions().trigger_hooks(HookType::PreRemove, txn);
                system_store_->remove(op.target().ns(), op.target().name());
                extensions().trigger_hooks(HookType::PostRemove, txn);
                break;
        }
    }

    if (!system_store_->save()) {
        system_store_->load();
        return fail_transaction("Failed to persist system store");
    }
    std::error_code ec;
    for (const auto& item : applied) {
        fs::remove_all(item.backup_root, ec);
        if (item.program_replaced) fs::remove_all(item.program_previous, ec);
    }
    for (const auto& op : pending_store) {
        if (op.type() != OperationType::Remove && op.type() != OperationType::Purge) continue;
        fs::remove_all(fs::path(data_dir_) / "pkg-scripts" / op.target().ns() / op.target().name(), ec);
    }

    system_store_->advance_state_id();
    txn.advance_state(TransactionState::Committed);

    extensions().trigger_hooks(HookType::PostTransaction, txn);
    record_transaction(txn);
    return true;
}

void LunarCore::record_transaction(const Transaction& txn) {
    transaction_history_.push_back(txn);
}

}
