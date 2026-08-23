#pragma once

#include "version.h"
#include "object.h"
#include "object_ref.h"
#include "collection.h"
#include "operation.h"
#include "transaction.h"
#include "repository.h"
#include "resolver.h"
#include "system_store.h"
#include "snapshot.h"
#include "extension_api.h"
#include <string>
#include <memory>

namespace okrapm {

// LunarCore: Lunar 核心引擎
// 统一管理所有组件: Repository, Resolver, SystemStore, Snapshot, Extension
// 核心保持尽可能小:
//   Object Model, Repository API, Dependency Resolver, Transaction Engine,
//   System Store, Collection Engine, Extension API
class LunarCore {
public:
    // 初始化核心
    LunarCore(const std::string& data_dir = "/var/lib/lunar");

    // ---- 组件访问 ----
    RepositoryManager& repositories() { return *repo_mgr_; }
    const RepositoryManager& repositories() const { return *repo_mgr_; }

    Resolver& resolver() { return resolver_; }
    const Resolver& resolver() const { return resolver_; }

    SystemStore& system_store() { return *system_store_; }
    const SystemStore& system_store() const { return *system_store_; }

    SnapshotManager& snapshots() { return *snapshot_mgr_; }
    const SnapshotManager& snapshots() const { return *snapshot_mgr_; }

    ExtensionApi& extensions() { return ExtensionApi::instance(); }

    // ---- 核心操作 ----

    // 安装对象
    struct InstallResult {
        bool success{false};
        Transaction transaction;
        std::string error_message;
    };
    InstallResult install(const std::vector<std::string>& refs, bool plan_only = false);

    // 删除对象
    struct RemoveResult {
        bool success{false};
        Transaction transaction;
        std::string error_message;
    };
    RemoveResult remove(const std::vector<std::string>& refs, bool purge = false, bool plan_only = false);

    // 同步
    InstallResult sync(const std::vector<std::string>& targets = {});

    // 更新 (Rolling Release 核心操作)
    InstallResult update(const std::vector<std::string>& refs = {}, bool plan_only = false);

    // 系统级升级
    InstallResult upgradle(const std::vector<std::string>& targets, bool plan_only = false);

    // 仅下载对象 Artifact
    struct DownloadResult {
        bool success{false};
        std::vector<std::string> downloaded_paths;
        std::string error_message;
    };
    DownloadResult download(const std::vector<std::string>& refs, const std::string& dest_dir = "");

    // 查询
    Collection<Object> find(const std::string& pattern) const;
    Collection<Object> search(const std::string& query) const;
    Collection<Object> list_installed() const;
    std::optional<Object> info(const std::string& ref) const;

    // 系统状态
    struct SystemStatus {
        uint64_t state_id{0};
        size_t installed_count{0};
        size_t outdated_count{0};
        std::vector<std::string> repositories;
        std::string system_version;
    };
    SystemStatus status() const;

    // 事务历史
    std::vector<Transaction> transaction_history() const;
    std::optional<Transaction> get_transaction(uint64_t id) const;

    // 快照操作
    Snapshot create_snapshot(const std::string& description = "");
    bool rollback(uint64_t snapshot_id);

    // 数据目录
    const std::string& data_dir() const { return data_dir_; }

private:
    std::string data_dir_;
    std::unique_ptr<RepositoryManager> repo_mgr_;
    Resolver resolver_;
    std::unique_ptr<SystemStore> system_store_;
    std::unique_ptr<SnapshotManager> snapshot_mgr_;
    std::vector<Transaction> transaction_history_;

    // 提交事务
    bool commit_transaction(Transaction& txn);

    // 记录事务
    void record_transaction(const Transaction& txn);
};

} // namespace okrapm
