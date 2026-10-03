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

class Resolver {
public:
    Resolver() = default;

    void set_repository_manager(RepositoryManager* repo_mgr) { repo_mgr_ = repo_mgr; }

    void set_installed_objects(std::vector<Object> installed) { installed_ = std::move(installed); }

    struct ResolveResult {
        bool success{false};
        std::vector<Operation> operations;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;

        size_t total_packages{0};
        size_t new_packages{0};
        size_t updated_packages{0};
    };

    ResolveResult resolve_install(const std::vector<ObjectRef>& refs);

    ResolveResult resolve_remove(const std::vector<ObjectRef>& refs, bool purge = false);

    ResolveResult resolve_update(const std::vector<ObjectRef>& refs = {});

    ResolveResult resolve_sync(const std::vector<ObjectRef>& refs = {});

    ResolveResult resolve_upgrade(const std::vector<ObjectRef>& refs);

    bool has_circular_dependency(const std::string& ns, const std::string& name,
                                 std::unordered_set<std::string>& visited) const;

    struct DependencyNode {
        Object object;
        std::vector<DependencyNode> children;
    };

    DependencyNode build_dependency_graph(const Object& root);

    std::vector<Object> reverse_dependencies(const std::string& ns, const std::string& name) const;

private:
    RepositoryManager* repo_mgr_{nullptr};
    std::vector<Object> installed_;

    bool is_installed(const std::string& ns, const std::string& name) const;
    std::optional<Object> get_installed(const std::string& ns, const std::string& name) const;

    std::vector<Object> topological_sort(const std::vector<Object>& objects);

    std::optional<Object> resolve_ref(const ObjectRef& ref) const;
};

}
