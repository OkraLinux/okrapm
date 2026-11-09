#pragma once

#include "version.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace okrapm {

enum class ObjectType {
    Package,
    Group,
    System,
    Artifact,
    Repository,
};

enum class ObjectState {
    Available,
    Installed,
    Outdated,
    Missing,
    Broken,
};

class Object {
public:
    Object() = default;
    virtual ~Object() = default;

    Object(std::string ns, std::string name, Version version = {},
           ObjectType type = ObjectType::Package);

    const std::string& ns() const { return ns_; }
    const std::string& name() const { return name_; }
    const Version& version() const { return version_; }
    ObjectType type() const { return type_; }
    ObjectState state() const { return state_; }

    std::string full_name() const;
    std::string ref_string() const;

    const std::string& repository() const { return repository_; }
    void set_repository(const std::string& repo) { repository_ = repo; }

    const std::string& description() const { return description_; }
    void set_description(const std::string& desc) { description_ = desc; }

    const std::vector<std::string>& dependencies() const { return dependencies_; }
    void set_dependencies(std::vector<std::string> deps) { dependencies_ = std::move(deps); }
    void add_dependency(std::string dep) { dependencies_.push_back(std::move(dep)); }

    const std::vector<std::string>& files() const { return files_; }
    void set_files(std::vector<std::string> files) { files_ = std::move(files); }

    void set_state(ObjectState state) { state_ = state; }
    void set_version(Version v) { version_ = std::move(v); }

    size_t download_size() const { return download_size_; }
    void set_download_size(size_t size) { download_size_ = size; }
    size_t installed_size() const { return installed_size_; }
    void set_installed_size(size_t size) { installed_size_ = size; }

    bool operator<(const Object& other) const {
        if (ns_ != other.ns_) return ns_ < other.ns_;
        if (name_ != other.name_) return name_ < other.name_;
        return version_ < other.version_;
    }
    bool operator==(const Object& other) const {
        return ns_ == other.ns_ && name_ == other.name_ && version_ == other.version_ && type_ == other.type_;
    }
    bool operator!=(const Object& other) const {
        return !(*this == other);
    }

    virtual std::string serialize() const;
    static std::optional<Object> deserialize(const std::string& data);

    static char type_prefix(ObjectType type);
    static std::string type_name(ObjectType type);

protected:
    std::string ns_;
    std::string name_;
    Version version_;
    ObjectType type_{ObjectType::Package};
    ObjectState state_{ObjectState::Available};
    std::string repository_;
    std::string description_;
    size_t download_size_{1024 * 1024 * 15};
    size_t installed_size_{1024 * 1024 * 45};
    std::vector<std::string> dependencies_;
    std::vector<std::string> files_;
};

class Package : public Object {
public:
    Package() { type_ = ObjectType::Package; }
    Package(std::string ns, std::string name, Version version = {}, std::string desc = "")
        : Object(std::move(ns), std::move(name), std::move(version), ObjectType::Package) {
        description_ = std::move(desc);
    }
};

class Group : public Object {
public:
    Group() { type_ = ObjectType::Group; }
    Group(std::string ns, std::string name, Version version = {}, std::string desc = "")
        : Object(std::move(ns), std::move(name), std::move(version), ObjectType::Group) {
        description_ = std::move(desc);
    }

    void add_member(std::string member) { dependencies_.push_back(std::move(member)); }
    const std::vector<std::string>& members() const { return dependencies_; }
    void set_members(std::vector<std::string> m) { dependencies_ = std::move(m); }
};

class SystemObject : public Object {
public:
    SystemObject() { type_ = ObjectType::System; }
    SystemObject(std::string name, Version version = {})
        : Object("okra", std::move(name), std::move(version), ObjectType::System) {}
};

class Artifact : public Object {
public:
    Artifact() { type_ = ObjectType::Artifact; }
    Artifact(std::string path);

    const std::string& path() const { return artifact_path_; }
    void set_path(const std::string& path) { artifact_path_ = path; }

    std::string serialize() const override;
    static std::optional<Artifact> deserialize(const std::string& data);

private:
    std::string artifact_path_;
};

}
