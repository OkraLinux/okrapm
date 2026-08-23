#include "okrapmlib/resolver.h"
#include <queue>
#include <algorithm>

namespace okrapm {

bool Resolver::is_installed(const std::string& ns, const std::string& name) const {
    for (const auto& obj : installed_) {
        if (obj.name() == name && (ns.empty() || obj.ns() == ns)) {
            return true;
        }
    }
    return false;
}

std::optional<Object> Resolver::get_installed(const std::string& ns, const std::string& name) const {
    for (const auto& obj : installed_) {
        if (obj.name() == name && (ns.empty() || obj.ns() == ns)) {
            return obj;
        }
    }
    return std::nullopt;
}

std::optional<Object> Resolver::resolve_ref(const ObjectRef& ref) const {
    if (ref.type() == ObjectType::Artifact) {
        // Artifact directly wraps a local file
        return Artifact(ref.artifact_path());
    }
    if (!repo_mgr_) return std::nullopt;
    return repo_mgr_->find_by_ref(ref);
}

bool Resolver::has_circular_dependency(const std::string& ns, const std::string& name,
                                       std::unordered_set<std::string>& visited) const {
    std::string key = ns + "." + name;
    if (visited.count(key)) return true;

    visited.insert(key);

    if (repo_mgr_) {
        auto obj = repo_mgr_->find(ns, name);
        if (obj) {
            for (const auto& dep_str : obj->dependencies()) {
                auto dep_ref = ObjectRef::parse(dep_str);
                if (dep_ref) {
                    if (has_circular_dependency(dep_ref->ns(), dep_ref->name(), visited)) {
                        return true;
                    }
                }
            }
        }
    }

    visited.erase(key);
    return false;
}

std::vector<Object> Resolver::reverse_dependencies(const std::string& ns, const std::string& name) const {
    std::vector<Object> result;
    std::string target_full = (ns.empty() ? "" : ns + ".") + name;

    for (const auto& obj : installed_) {
        for (const auto& dep : obj.dependencies()) {
            auto dep_ref = ObjectRef::parse(dep);
            if (dep_ref && dep_ref->name() == name && (ns.empty() || dep_ref->ns().empty() || dep_ref->ns() == ns)) {
                result.push_back(obj);
                break;
            }
        }
    }
    return result;
}

Resolver::ResolveResult Resolver::resolve_install(const std::vector<ObjectRef>& refs) {
    ResolveResult result;

    if (!repo_mgr_) {
        result.errors.push_back("No repository manager configured");
        return result;
    }

    std::vector<Object> to_install;
    std::unordered_set<std::string> queued;
    std::queue<ObjectRef> queue;

    for (const auto& ref : refs) {
        queue.push(ref);
    }

    while (!queue.empty()) {
        auto current_ref = queue.front();
        queue.pop();

        std::string key = current_ref.ns() + "." + current_ref.name();
        if (queued.count(key)) continue;

        // Group handling: expand group members into queue
        if (current_ref.type() == ObjectType::Group) {
            auto group_obj = repo_mgr_->find_by_ref(current_ref);
            if (!group_obj) {
                result.errors.push_back("Group not found: " + current_ref.to_string());
                continue;
            }
            for (const auto& member : group_obj->dependencies()) {
                auto member_ref = ObjectRef::parse(member);
                if (member_ref) queue.push(*member_ref);
            }
            continue;
        }

        // Artifact handling
        if (current_ref.type() == ObjectType::Artifact) {
            Artifact art(current_ref.artifact_path());
            to_install.push_back(art);
            queued.insert(art.full_name());
            continue;
        }

        // Resolve object from repos
        auto obj = resolve_ref(current_ref);
        if (!obj) {
            result.errors.push_back("Object not found: " + current_ref.to_string());
            continue;
        }

        // Check if already installed with same version
        auto installed_obj = get_installed(obj->ns(), obj->name());
        if (installed_obj) {
            if (installed_obj->version() >= obj->version()) {
                // Already satisfied
                continue;
            }
            // Will update
            result.updated_packages++;
            Operation op(OperationType::Update, *obj, *installed_obj);
            result.operations.push_back(op);
        } else {
            result.new_packages++;
            to_install.push_back(*obj);
        }
        queued.insert(key);

        // Circular check
        std::unordered_set<std::string> circ_visited;
        if (has_circular_dependency(obj->ns(), obj->name(), circ_visited)) {
            result.warnings.push_back("Potential circular dependency in: " + obj->full_name());
        }

        // Add dependencies to queue
        for (const auto& dep_str : obj->dependencies()) {
            auto dep_ref = ObjectRef::parse(dep_str);
            if (dep_ref) {
                std::string dep_key = dep_ref->ns() + "." + dep_ref->name();
                if (!queued.count(dep_key) && !is_installed(dep_ref->ns(), dep_ref->name())) {
                    queue.push(*dep_ref);
                }
            }
        }
    }

    if (!result.errors.empty()) {
        result.success = false;
        return result;
    }

    // Topological sort for install operations (dependencies first)
    auto sorted = topological_sort(to_install);
    for (const auto& obj : sorted) {
        result.operations.push_back(Operation(OperationType::Install, obj));
    }

    result.total_packages = result.operations.size();
    result.success = true;
    return result;
}

Resolver::ResolveResult Resolver::resolve_remove(const std::vector<ObjectRef>& refs, bool purge) {
    ResolveResult result;

    std::vector<Object> to_remove;
    std::unordered_set<std::string> queued;

    for (const auto& ref : refs) {
        auto inst = get_installed(ref.ns(), ref.name());
        if (!inst) {
            result.errors.push_back("Object not installed: " + ref.to_string());
            continue;
        }

        // Check reverse dependencies
        auto rev_deps = reverse_dependencies(inst->ns(), inst->name());
        if (!rev_deps.empty() && !purge) {
            std::string msg = "Cannot remove " + inst->full_name() + ", depended upon by:";
            for (const auto& rd : rev_deps) {
                msg += " " + rd.full_name();
            }
            result.errors.push_back(msg);
            continue;
        }

        if (purge) {
            // Cascade remove all dependents
            std::queue<Object> cascade_queue;
            cascade_queue.push(*inst);
            while (!cascade_queue.empty()) {
                auto curr = cascade_queue.front();
                cascade_queue.pop();
                std::string key = curr.full_name();
                if (queued.count(key)) continue;
                queued.insert(key);
                to_remove.push_back(curr);

                auto rdeps = reverse_dependencies(curr.ns(), curr.name());
                for (const auto& rd : rdeps) {
                    cascade_queue.push(rd);
                }
            }
        } else {
            to_remove.push_back(*inst);
            queued.insert(inst->full_name());
        }
    }

    if (!result.errors.empty()) {
        result.success = false;
        return result;
    }

    OperationType op_type = purge ? OperationType::Purge : OperationType::Remove;
    for (const auto& obj : to_remove) {
        result.operations.push_back(Operation(op_type, obj));
    }

    result.total_packages = result.operations.size();
    result.success = true;
    return result;
}

Resolver::ResolveResult Resolver::resolve_update(const std::vector<ObjectRef>& refs) {
    ResolveResult result;
    if (!repo_mgr_) {
        result.errors.push_back("No repository manager configured");
        return result;
    }

    std::vector<ObjectRef> targets = refs;

    // If no targets given, update all installed objects that are outdated
    if (targets.empty()) {
        for (const auto& inst : installed_) {
            auto avail = repo_mgr_->find(inst.ns(), inst.name());
            if (avail && avail->version() > inst.version()) {
                auto ref = ObjectRef::parse(inst.full_name());
                if (ref) targets.push_back(*ref);
            }
        }
    }

    if (targets.empty()) {
        result.success = true;
        result.warnings.push_back("Everything is up to date");
        return result;
    }

    return resolve_install(targets);
}

Resolver::ResolveResult Resolver::resolve_sync(const std::vector<ObjectRef>& refs) {
    ResolveResult result;
    if (refs.empty()) {
        // Sync everything
        if (repo_mgr_) repo_mgr_->sync_all();
    } else {
        for (const auto& ref : refs) {
            if (ref.type() == ObjectType::System) {
                // Sync system object
                if (repo_mgr_) {
                    auto obj = repo_mgr_->find_by_ref(ref);
                    if (obj) {
                        result.operations.push_back(Operation(OperationType::Sync, *obj));
                    }
                }
            } else {
                // Sync specific repo
                if (repo_mgr_) repo_mgr_->sync_repository(ref.name());
            }
        }
    }
    result.success = true;
    return result;
}

Resolver::ResolveResult Resolver::resolve_upgrade(const std::vector<ObjectRef>& refs) {
    ResolveResult result;
    for (const auto& ref : refs) {
        auto obj = resolve_ref(ref);
        if (obj) {
            auto inst = get_installed(obj->ns(), obj->name());
            result.operations.push_back(Operation(OperationType::Upgrade, *obj, inst ? *inst : Object{}));
        } else {
            result.errors.push_back("Upgrade target not found: " + ref.to_string());
        }
    }
    result.success = result.errors.empty();
    return result;
}

std::vector<Object> Resolver::topological_sort(const std::vector<Object>& objects) {
    std::vector<Object> sorted;
    std::unordered_set<std::string> visited;

    std::function<void(const Object&)> visit = [&](const Object& obj) {
        std::string key = obj.full_name();
        if (visited.count(key)) return;
        visited.insert(key);

        for (const auto& dep_str : obj.dependencies()) {
            auto dep_ref = ObjectRef::parse(dep_str);
            if (dep_ref) {
                // Find in our objects list
                for (const auto& candidate : objects) {
                    if (candidate.name() == dep_ref->name() &&
                        (dep_ref->ns().empty() || candidate.ns() == dep_ref->ns())) {
                        visit(candidate);
                        break;
                    }
                }
            }
        }
        sorted.push_back(obj);
    };

    for (const auto& obj : objects) {
        visit(obj);
    }
    return sorted;
}

} // namespace okrapm
