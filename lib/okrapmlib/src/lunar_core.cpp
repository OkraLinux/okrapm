#include "okrapmlib/lunar_core.h"
#include "okrapmlib/artifact_engine.h"
#include <filesystem>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace okrapm {

LunarCore::LunarCore(const std::string& data_dir)
    : data_dir_(data_dir) {
    std::error_code ec;
    fs::create_directories(data_dir_, ec);
    fs::create_directories(data_dir_ + "/repos", ec);
    fs::create_directories(data_dir_ + "/snapshots", ec);
    fs::create_directories(data_dir_ + "/extensions", ec);

    repo_mgr_ = std::make_unique<RepositoryManager>(data_dir_ + "/repos");
    system_store_ = std::make_unique<SystemStore>(data_dir_ + "/system.db");
    snapshot_mgr_ = std::make_unique<SnapshotManager>(data_dir_ + "/snapshots");

    resolver_.set_repository_manager(repo_mgr_.get());
    resolver_.set_installed_objects(system_store_->list_installed());

    // 注册内置扩展
    ExtensionInfo core_ext;
    core_ext.name = "lunar-core";
    core_ext.version = "1.0.0";
    core_ext.description = "Lunar native core engine";
    core_ext.type = ExtensionType::ObjectType;
    extensions().register_extension(core_ext);

    ExtensionInfo oaa_backend;
    oaa_backend.name = "lunar-oaa";
    oaa_backend.version = "1.0.0";
    oaa_backend.description = "Lunar Native Artifact (.oaa) Backend";
    oaa_backend.type = ExtensionType::ArtifactBackend;
    extensions().register_extension(oaa_backend);

    ExtensionInfo okra_backend;
    okra_backend.name = "okrapm";
    okra_backend.version = "3.0.0";
    okra_backend.description = "Okra Package Format (.okra) & Legacy okpm Compatibility Loader";
    okra_backend.type = ExtensionType::ArtifactBackend;
    extensions().register_extension(okra_backend);

    // 自动加载系统和用户插件
    extensions().load_plugins_from_directory(data_dir_ + "/extensions");
    extensions().load_plugins_from_directory("/usr/lib/lunar/extensions");
    extensions().load_plugins_from_directory("/var/lib/okpm/extensions");
}

LunarCore::InstallResult LunarCore::install(const std::vector<std::string>& refs, bool plan_only) {
    InstallResult res;
    if (refs.empty()) {
        res.error_message = "No objects specified to install";
        return res;
    }

    std::vector<ObjectRef> parsed_refs;
    std::vector<Object> artifact_objects;

    for (const auto& r : refs) {
        auto parsed = ObjectRef::parse(r);
        if (!parsed) {
            res.error_message = "Invalid object reference: " + r;
            return res;
        }

        if (parsed->is_artifact()) {
            // 本地 Artifact 文件安装
            auto meta = ArtifactExtractor::inspect(parsed->artifact_path());
            if (!meta) {
                res.error_message = "Failed to inspect artifact archive: " + parsed->artifact_path();
                return res;
            }
            auto obj = meta->to_object();
            artifact_objects.push_back(obj);
        } else {
            parsed_refs.push_back(*parsed);
        }
    }

    resolver_.set_installed_objects(system_store_->list_installed());
    std::vector<Operation> all_ops;

    if (!parsed_refs.empty()) {
        auto resolve_res = resolver_.resolve_install(parsed_refs);
        if (!resolve_res.success) {
            res.error_message = resolve_res.errors.empty() ? "Dependency resolution failed" : resolve_res.errors[0];
            return res;
        }
        all_ops = resolve_res.operations;
    }

    for (const auto& art_obj : artifact_objects) {
        auto current = system_store_->find(art_obj.ns(), art_obj.name());
        if (current) {
            all_ops.emplace_back(OperationType::Update, art_obj, *current);
        } else {
            all_ops.emplace_back(OperationType::Install, art_obj);
        }
    }

    res.transaction = Transaction(all_ops);
    res.transaction.set_description("Install objects");
    res.transaction.set_id(system_store_->next_state_id());
    res.transaction.advance_state(TransactionState::Planned);

    if (plan_only) {
        res.success = true;
        return res;
    }

    res.success = commit_transaction(res.transaction);
    if (!res.success) {
        res.error_message = "Transaction commit failed: " + res.transaction.error_message();
    }
    return res;
}

LunarCore::RemoveResult LunarCore::remove(const std::vector<std::string>& refs, bool purge, bool plan_only) {
    RemoveResult res;
    if (refs.empty()) {
        res.error_message = "No objects specified to remove";
        return res;
    }

    std::vector<ObjectRef> parsed_refs;
    for (const auto& r : refs) {
        auto parsed = ObjectRef::parse(r);
        if (parsed) parsed_refs.push_back(*parsed);
    }

    resolver_.set_installed_objects(system_store_->list_installed());
    auto resolve_res = resolver_.resolve_remove(parsed_refs, purge);
    if (!resolve_res.success) {
        res.error_message = resolve_res.errors.empty() ? "Dependency resolution failed" : resolve_res.errors[0];
        return res;
    }

    res.transaction = Transaction(resolve_res.operations);
    res.transaction.set_description(purge ? "Purge objects" : "Remove objects");
    res.transaction.set_id(system_store_->next_state_id());
    res.transaction.advance_state(TransactionState::Planned);

    if (plan_only) {
        res.success = true;
        return res;
    }

    res.success = commit_transaction(res.transaction);
    if (!res.success) {
        res.error_message = "Transaction commit failed: " + res.transaction.error_message();
    }
    return res;
}

LunarCore::InstallResult LunarCore::sync(const std::vector<std::string>& targets) {
    InstallResult res;
    Transaction txn;
    txn.set_description("Sync repositories and objects");

    if (targets.empty()) {
        // 同步所有仓库
        for (const auto& repo : repo_mgr_->list()) {
            repo->sync();
        }
        res.success = true;
        return res;
    }

    for (const auto& target : targets) {
        auto repo = repo_mgr_->get(target);
        if (repo) {
            repo->sync();
        } else {
            // 同步指定系统对象或命名空间
            auto obj_opt = repo_mgr_->find(target);
            if (obj_opt) {
                Operation op(OperationType::Sync, *obj_opt);
                txn.add_operation(op);
            }
        }
    }

    if (!txn.operations().empty()) {
        res.transaction = txn;
        res.success = commit_transaction(res.transaction);
    } else {
        res.success = true;
    }
    return res;
}

LunarCore::InstallResult LunarCore::update(const std::vector<std::string>& refs, bool plan_only) {
    InstallResult res;
    std::vector<ObjectRef> parsed_refs;
    for (const auto& r : refs) {
        auto parsed = ObjectRef::parse(r);
        if (parsed) parsed_refs.push_back(*parsed);
    }

    resolver_.set_installed_objects(system_store_->list_installed());
    auto resolve_res = resolver_.resolve_update(parsed_refs);
    if (!resolve_res.success) {
        res.error_message = resolve_res.errors.empty() ? "Dependency resolution failed" : resolve_res.errors[0];
        return res;
    }

    res.transaction = Transaction(resolve_res.operations);
    res.transaction.set_description("Rolling system update");
    res.transaction.set_id(system_store_->next_state_id());
    res.transaction.advance_state(TransactionState::Planned);

    if (plan_only) {
        res.success = true;
        return res;
    }

    res.success = commit_transaction(res.transaction);
    if (!res.success) {
        res.error_message = "Transaction commit failed: " + res.transaction.error_message();
    }
    return res;
}

LunarCore::InstallResult LunarCore::upgradle(const std::vector<std::string>& targets, bool plan_only) {
    InstallResult res;
    std::vector<ObjectRef> parsed_refs;
    for (const auto& r : targets) {
        auto parsed = ObjectRef::parse(r);
        if (parsed) parsed_refs.push_back(*parsed);
    }

    auto resolve_res = resolver_.resolve_upgrade(parsed_refs);
    if (!resolve_res.success) {
        res.error_message = resolve_res.errors.empty() ? "Upgrade resolution failed" : resolve_res.errors[0];
        return res;
    }

    res.transaction = Transaction(resolve_res.operations);
    res.transaction.set_description("System-level baseline upgrade (Upgradle)");
    res.transaction.set_id(system_store_->next_state_id());
    res.transaction.advance_state(TransactionState::Planned);

    if (plan_only) {
        res.success = true;
        return res;
    }

    res.success = commit_transaction(res.transaction);
    if (!res.success) {
        res.error_message = "Transaction commit failed: " + res.transaction.error_message();
    }
    return res;
}

LunarCore::DownloadResult LunarCore::download(const std::vector<std::string>& refs, const std::string& dest_dir) {
    DownloadResult res;
    std::string target_dir = dest_dir.empty() ? (data_dir_ + "/cache/downloads") : dest_dir;
    std::error_code ec;
    fs::create_directories(target_dir, ec);

    for (const auto& r : refs) {
        auto parsed = ObjectRef::parse(r);
        if (!parsed) {
            res.error_message = "Invalid object ref: " + r;
            return res;
        }

        auto obj = repo_mgr_->find_by_ref(*parsed);
        if (!obj) {
            res.error_message = "Object not found in repositories: " + r;
            return res;
        }

        auto repo = repo_mgr_->get_repository_for_object(*obj);
        if (!repo) {
            res.error_message = "No repository found for object: " + r;
            return res;
        }

        auto path = repo->fetch_artifact(*obj, target_dir);
        if (path) {
            res.downloaded_paths.push_back(*path);
        } else {
            res.error_message = "Failed to download artifact for " + r;
            return res;
        }
    }

    res.success = true;
    return res;
}

Collection<Object> LunarCore::find(const std::string& pattern) const {
    auto all_objs = repo_mgr_->all_objects();
    return all_objs.where([&](const Object& obj) {
        return ObjectRef::matches_pattern(pattern, obj.full_name()) ||
               ObjectRef::matches_pattern(pattern, obj.ns()) ||
               ObjectRef::matches_pattern(pattern, obj.name());
    });
}

Collection<Object> LunarCore::search(const std::string& query) const {
    auto all_objs = repo_mgr_->all_objects();
    return all_objs.where([&](const Object& obj) {
        return obj.name().find(query) != std::string::npos ||
               obj.ns().find(query) != std::string::npos ||
               obj.description().find(query) != std::string::npos;
    });
}

Collection<Object> LunarCore::list_installed() const {
    return system_store_->collection();
}

std::optional<Object> LunarCore::info(const std::string& ref) const {
    auto parsed = ObjectRef::parse(ref);
    if (!parsed) return std::nullopt;

    auto inst = system_store_->find(parsed->ns(), parsed->name());
    if (inst) return inst;

    return repo_mgr_->find(parsed->ns(), parsed->name());
}

LunarCore::SystemStatus LunarCore::status() const {
    SystemStatus st;
    st.state_id = system_store_->next_state_id() - 1;
    st.installed_count = system_store_->list_installed().size();

    // 计算过期数
    size_t outdated = 0;
    for (const auto& inst : system_store_->list_installed()) {
        auto latest = repo_mgr_->find(inst.ns(), inst.name());
        if (latest && latest->version() > inst.version()) {
            ++outdated;
        }
    }
    st.outdated_count = outdated;

    for (const auto& repo : repo_mgr_->list()) {
        st.repositories.push_back(repo->name());
    }

    auto sys_ver = system_store_->find("okra", "systemversion");
    if (sys_ver) {
        st.system_version = sys_ver->version().to_string();
    } else {
        st.system_version = "Rolling State";
    }

    return st;
}

std::vector<Transaction> LunarCore::transaction_history() const {
    return transaction_history_;
}

std::optional<Transaction> LunarCore::get_transaction(uint64_t id) const {
    for (const auto& txn : transaction_history_) {
        if (txn.id() == id) return txn;
    }
    return std::nullopt;
}

Snapshot LunarCore::create_snapshot(const std::string& description) {
    return snapshot_mgr_->create(*system_store_, description);
}

bool LunarCore::rollback(uint64_t snapshot_id) {
    auto plan = snapshot_mgr_->restore_plan(snapshot_id, *system_store_);
    Transaction txn;
    txn.set_description("Rollback to Snapshot #" + std::to_string(snapshot_id));

    for (const auto& obj : plan.to_remove) {
        txn.add_operation(Operation(OperationType::Remove, obj));
    }
    for (const auto& obj : plan.to_install) {
        auto cur = system_store_->find(obj.ns(), obj.name());
        if (cur) {
            txn.add_operation(Operation(OperationType::Update, obj, *cur));
        } else {
            txn.add_operation(Operation(OperationType::Install, obj));
        }
    }

    bool success = commit_transaction(txn);
    if (success) {
        success = snapshot_mgr_->restore(snapshot_id, *system_store_);
    }
    return success;
}

bool LunarCore::commit_transaction(Transaction& txn) {
    txn.advance_state(TransactionState::Verified);
    extensions().trigger_hooks(HookType::PreTransaction, txn);

    txn.advance_state(TransactionState::Committing);

    // 事务前自动生成快照保护系统
    snapshot_mgr_->create(*system_store_, "Auto snapshot before txn " + std::to_string(txn.id()));

    for (const auto& op : txn.operations()) {
        switch (op.type()) {
            case OperationType::Install:
            case OperationType::Update:
            case OperationType::Upgrade:
            case OperationType::Sync: {
                extensions().trigger_hooks(HookType::PreInstall, txn);
                auto repo = repo_mgr_->get_repository_for_object(op.target());
                if (repo) {
                    auto art_path = repo->fetch_artifact(op.target());
                    if (art_path && fs::exists(*art_path)) {
                        ArtifactExtractor::extract(*art_path, data_dir_ + "/installed/" + op.target().full_name());
                    }
                }
                system_store_->install(op.target());
                extensions().trigger_hooks(HookType::PostInstall, txn);
                break;
            }
            case OperationType::Remove:
            case OperationType::Purge:
                extensions().trigger_hooks(HookType::PreRemove, txn);
                system_store_->remove(op.target().ns(), op.target().name());
                extensions().trigger_hooks(HookType::PostRemove, txn);
                break;
        }
    }

    if (!system_store_->save()) {
        txn.advance_state(TransactionState::Failed, "Failed to persist system store");
        txn.advance_state(TransactionState::RolledBack);
        return false;
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

} // namespace okrapm
