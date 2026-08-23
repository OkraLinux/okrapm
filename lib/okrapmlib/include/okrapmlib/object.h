#pragma once

#include "version.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace okrapm {

// 对象类型枚举
enum class ObjectType {
    Package,    // 普通软件包 gnu.gcc
    Group,      // 对象组 #kde.kde-desktop
    System,     // 系统对象 okra.systemkernel
    Artifact,   // 安装载体 app.oaa
    Repository, // 仓库对象
};

// 对象状态
enum class ObjectState {
    Available,  // 仓库中可用
    Installed,  // 已安装
    Outdated,    // 已安装但有更新版本
    Missing,    // 未安装且仓库中不可用
    Broken,     // 已安装但依赖缺失
};

// 对象基类 —— Lunar 中的一切都是 Object
// 标准引用形式: namespace.name@version
class Object {
public:
    Object() = default;
    virtual ~Object() = default;

    Object(std::string ns, std::string name, Version version = {},
           ObjectType type = ObjectType::Package);

    // 基本属性
    const std::string& ns() const { return ns_; }       // namespace
    const std::string& name() const { return name_; }
    const Version& version() const { return version_; }
    ObjectType type() const { return type_; }
    ObjectState state() const { return state_; }

    // 完整引用字符串: namespace.name@version
    std::string full_name() const;     // namespace.name
    std::string ref_string() const;    // namespace.name@version

    // 来源仓库
    const std::string& repository() const { return repository_; }
    void set_repository(const std::string& repo) { repository_ = repo; }

    // 描述
    const std::string& description() const { return description_; }
    void set_description(const std::string& desc) { description_ = desc; }

    // 依赖列表 (namespace.name 格式)
    const std::vector<std::string>& dependencies() const { return dependencies_; }
    void set_dependencies(std::vector<std::string> deps) { dependencies_ = std::move(deps); }
    void add_dependency(std::string dep) { dependencies_.push_back(std::move(dep)); }

    // 安装文件列表
    const std::vector<std::string>& files() const { return files_; }
    void set_files(std::vector<std::string> files) { files_ = std::move(files); }

    // 状态设置
    void set_state(ObjectState state) { state_ = state; }
    void set_version(Version v) { version_ = std::move(v); }

    // 大小属性 (字节)
    size_t download_size() const { return download_size_; }
    void set_download_size(size_t size) { download_size_ = size; }
    size_t installed_size() const { return installed_size_; }
    void set_installed_size(size_t size) { installed_size_ = size; }

    // 比较运算符
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

    // 序列化/反序列化 (用于数据库存储)
    virtual std::string serialize() const;
    static std::optional<Object> deserialize(const std::string& data);

    // 类型标记前缀
    static char type_prefix(ObjectType type);
    static std::string type_name(ObjectType type);

protected:
    std::string ns_;            // namespace, e.g. "gnu"
    std::string name_;          // name, e.g. "gcc"
    Version version_;           // 版本
    ObjectType type_{ObjectType::Package};
    ObjectState state_{ObjectState::Available};
    std::string repository_;    // 来源仓库
    std::string description_;   // 描述
    size_t download_size_{1024 * 1024 * 15}; // 默认 15MB 估算
    size_t installed_size_{1024 * 1024 * 45}; // 默认 45MB 估算
    std::vector<std::string> dependencies_;  // 依赖列表
    std::vector<std::string> files_;        // 文件列表
};

// Package: 普通软件包 (gnu.gcc 等)
class Package : public Object {
public:
    Package() { type_ = ObjectType::Package; }
    Package(std::string ns, std::string name, Version version = {}, std::string desc = "")
        : Object(std::move(ns), std::move(name), std::move(version), ObjectType::Package) {
        description_ = std::move(desc);
    }
};

// Group: 一组相关软件的集合
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

// SystemObject: 系统组件 (okra.systemkernel 等)
class SystemObject : public Object {
public:
    SystemObject() { type_ = ObjectType::System; }
    SystemObject(std::string name, Version version = {})
        : Object("okra", std::move(name), std::move(version), ObjectType::System) {}
};

// Artifact: 安装载体 (.oaa 文件等)
class Artifact : public Object {
public:
    Artifact() { type_ = ObjectType::Artifact; }
    Artifact(std::string path);

    const std::string& path() const { return artifact_path_; }
    void set_path(const std::string& path) { artifact_path_ = path; }

    std::string serialize() const override;
    static std::optional<Artifact> deserialize(const std::string& data);

private:
    std::string artifact_path_;  // 本地文件路径
};

} // namespace okrapm
