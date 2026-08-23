#pragma once

#include "object.h"
#include "object_ref.h"
#include "transaction.h"
#include "repository.h"
#include "collection.h"
#include <string>
#include <vector>
#include <optional>
#include <unordered_set>
#include <unordered_map>

namespace okrapm {

// Resolver: 依赖解析器
// 负责: Query -> Dependency Graph -> Transaction Plan
// Resolver 与 CLI 分离，可替换不同解析策略
class Resolver {
public:
    Resolver() = default;

    // 设置仓库管理器
    void set_repository_manager(RepositoryManager* repo_mgr) { repo_mgr_ = repo_mgr; }

    // 设置已安装对象列表 (用于检查已满足的依赖)
    void set_installed_objects(std::vector<Object> installed) { installed_ = std::move(installed); }

    // 解析安装请求 -> 生成事务操作列表
    // 返回需要安装的所有对象 (按拓扑顺序)
    struct ResolveResult {
        bool success{false};
        std::vector<Operation> operations;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;

        // 统计信息
        size_t total_packages{0};
        size_t new_packages{0};
        size_t updated_packages{0};
    };

    // 解析安装请求
    ResolveResult resolve_install(const std::vector<ObjectRef>& refs);

    // 解析删除请求
    ResolveResult resolve_remove(const std::vector<ObjectRef>& refs, bool purge = false);

    // 解析更新请求
    // 如果 refs 为空，则更新所有过期对象
    ResolveResult resolve_update(const std::vector<ObjectRef>& refs = {});

    // 解析同步请求
    ResolveResult resolve_sync(const std::vector<ObjectRef>& refs = {});

    // 解析系统级升级请求
    ResolveResult resolve_upgrade(const std::vector<ObjectRef>& refs);

    // 检测循环依赖
    bool has_circular_dependency(const std::string& ns, const std::string& name,
                                 std::unordered_set<std::string>& visited) const;

    // 构建依赖图
    struct DependencyNode {
        Object object;
        std::vector<DependencyNode> children;
    };

    DependencyNode build_dependency_graph(const Object& root);

    // 获取反向依赖 (哪些已安装的包依赖给定的包)
    std::vector<Object> reverse_dependencies(const std::string& ns, const std::string& name) const;

private:
    RepositoryManager* repo_mgr_{nullptr};
    std::vector<Object> installed_;

    // 检查对象是否已安装
    bool is_installed(const std::string& ns, const std::string& name) const;
    std::optional<Object> get_installed(const std::string& ns, const std::string& name) const;

    // 拓扑排序
    std::vector<Object> topological_sort(const std::vector<Object>& objects);

    // 将 ObjectRef 解析为具体 Object
    std::optional<Object> resolve_ref(const ObjectRef& ref) const;
};

} // namespace okrapm
