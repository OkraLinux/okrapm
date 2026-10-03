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

class LunarCore {
public:

    LunarCore(const std::string& data_dir = "/var/lib/lunar");

    RepositoryManager& repositories() { return *repo_mgr_; }
    const RepositoryManager& repositories() const { return *repo_mgr_; }

    Resolver& resolver() { return resolver_; }
    const Resolver& resolver() const { return resolver_; }

    SystemStore& system_store() { return *system_store_; }
    const SystemStore& system_store() const { return *system_store_; }

    SnapshotManager& snapshots() { return *snapshot_mgr_; }
    const SnapshotManager& snapshots() const { return *snapshot_mgr_; }

    ExtensionApi& extensions() { return ExtensionApi::instance(); }

    struct InstallResult {
        bool success{false};
        Transaction transaction;
        std::string error_message;
    };
    InstallResult install(const std::vector<std::string>& refs, bool plan_only = false);

    struct RemoveResult {
        bool success{false};
        Transaction transaction;
        std::string error_message;
    };
    RemoveResult remove(const std::vector<std::string>& refs, bool purge = false, bool plan_only = false);

    InstallResult sync(const std::vector<std::string>& targets = {});

    InstallResult update(const std::vector<std::string>& refs = {}, bool plan_only = false);

    InstallResult upgradle(const std::vector<std::string>& targets, bool plan_only = false);

    struct DownloadResult {
        bool success{false};
        std::vector<std::string> downloaded_paths;
        std::string error_message;
    };
    DownloadResult download(const std::vector<std::string>& refs, const std::string& dest_dir = "");

    Collection<Object> find(const std::string& pattern) const;
    Collection<Object> search(const std::string& query) const;
    Collection<Object> list_installed() const;
    std::optional<Object> info(const std::string& ref) const;

    struct SystemStatus {
        uint64_t state_id{0};
        size_t installed_count{0};
        size_t outdated_count{0};
        std::vector<std::string> repositories;
        std::string system_version;
    };
    SystemStatus status() const;

    std::vector<Transaction> transaction_history() const;
    std::optional<Transaction> get_transaction(uint64_t id) const;

    Snapshot create_snapshot(const std::string& description = "");
    bool rollback(uint64_t snapshot_id);

    const std::string& data_dir() const { return data_dir_; }

private:
    std::string data_dir_;
    std::unique_ptr<RepositoryManager> repo_mgr_;
    Resolver resolver_;
    std::unique_ptr<SystemStore> system_store_;
    std::unique_ptr<SnapshotManager> snapshot_mgr_;
    std::vector<Transaction> transaction_history_;

    bool commit_transaction(Transaction& txn);

    void record_transaction(const Transaction& txn);
};

}
