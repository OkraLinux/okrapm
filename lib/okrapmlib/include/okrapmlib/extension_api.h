#pragma once

#include "object.h"
#include "transaction.h"
#include "repository.h"
#include "resolver.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <unordered_map>

namespace okrapm {

enum class ExtensionType {
    ObjectType,
    Operation,
    RepositoryBackend,
    ArtifactBackend,
    Resolver,
    Hook,
    Formatter,
    Plugin,
};

struct ExtensionInfo {
    std::string name;
    std::string version;
    std::string description;
    ExtensionType type;
    std::string file_path;
};

enum class HookType {
    PreTransaction,
    PostTransaction,
    PreInstall,
    PostInstall,
    PreRemove,
    PostRemove,
};

class ExtensionApi;

using HookCallback = std::function<void(const Transaction&)>;

using PluginInitFunc = bool (*)(ExtensionApi*);
using PluginCleanupFunc = void (*)();

class ExtensionApi {
public:
    static ExtensionApi& instance();

    void register_extension(const ExtensionInfo& info);

    std::vector<ExtensionInfo> list_extensions() const;

    void register_hook(HookType type, HookCallback callback);

    void trigger_hooks(HookType type, const Transaction& txn) const;

    using OperationHandler = std::function<bool(const std::vector<std::string>& args)>;
    void register_operation(const std::string& name, const std::string& description,
                           OperationHandler handler);

    bool execute_operation(const std::string& name, const std::vector<std::string>& args) const;

    std::vector<std::pair<std::string, std::string>> list_operations() const;

    bool load_plugin(const std::string& so_path);
    size_t load_plugins_from_directory(const std::string& dir_path);
    void unload_all();

private:
    ExtensionApi() = default;
    ~ExtensionApi();

    struct LoadedPlugin {
        void* handle{nullptr};
        bool oaabi{false};
        void (*fini)(){nullptr};
    };

    bool load_oaabi_plugin(const std::string& so_path);

    std::vector<ExtensionInfo> extensions_;
    std::unordered_map<HookType, std::vector<HookCallback>> hooks_;
    std::unordered_map<std::string, std::pair<std::string, OperationHandler>> operations_;
    std::vector<LoadedPlugin> plugin_handles_;
};

}
