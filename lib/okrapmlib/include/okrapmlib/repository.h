#pragma once

#include "object.h"
#include "object_ref.h"
#include "collection.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>

namespace okrapm {

// Repository: 仓库抽象接口
// Repository 不应与 Package Model 强绑定
// Lunar Core 只需要定义统一 Repository API:
//   Repository -> Index -> Objects -> Artifacts
class Repository {
public:
    virtual ~Repository() = default;

    // 仓库元数据
    virtual std::string name() const = 0;
    virtual std::string url() const = 0;
    virtual bool enabled() const = 0;
    virtual void set_enabled(bool enabled) = 0;

    // 查找对象
    virtual std::optional<Object> find(const std::string& ns, const std::string& name) const = 0;
    virtual std::optional<Object> find_by_ref(const ObjectRef& ref) const = 0;

    // 列出所有对象
    virtual std::vector<Object> list_objects() const = 0;

    // 搜索对象 (返回 Collection<Object>)
    virtual Collection<Object> search(const std::string& query) const = 0;

    // 通配符模式匹配
    virtual Collection<Object> find_pattern(const std::string& pattern) const = 0;

    // 仓库类型名称
    virtual std::string type_name() const = 0;

    // 同步 (从远程更新本地索引)
    virtual bool sync() = 0;

    // 持久化
    virtual bool save() const = 0;
    virtual bool load() = 0;

    // 获取并下载软件包 Artifact 到本地，返回本地文件路径
    virtual std::optional<std::string> fetch_artifact(const Object& obj, const std::string& dest_dir = "") {
        (void)obj; (void)dest_dir;
        return std::nullopt;
    }
};

// LocalRepository: 本地文件仓库后端
// 使用简单的文本数据库格式
class LocalRepository : public Repository {
public:
    explicit LocalRepository(const std::string& name, const std::string& db_path,
                             const std::string& url = "", bool enabled = true);

    std::string name() const override { return name_; }
    std::string url() const override { return url_; }
    bool enabled() const override { return enabled_; }
    void set_enabled(bool enabled) override { enabled_ = enabled; }

    std::optional<Object> find(const std::string& ns, const std::string& name) const override;
    std::optional<Object> find_by_ref(const ObjectRef& ref) const override;
    std::vector<Object> list_objects() const override;
    Collection<Object> search(const std::string& query) const override;
    Collection<Object> find_pattern(const std::string& pattern) const override;

    std::string type_name() const override { return "local"; }

    bool sync() override;
    bool save() const override;
    bool load() override;

    // 添加/删除对象 (用于仓库管理)
    void add_object(const Object& obj);
    void remove_object(const std::string& ns, const std::string& name);

private:
    std::string name_;
    std::string db_path_;
    std::string url_;
    bool enabled_{true};
    std::vector<Object> objects_;
};

// RemoteRepository: 远程 HTTP / HTTPS 仓库后端
class RemoteRepository : public Repository {
public:
    explicit RemoteRepository(const std::string& name, const std::string& url,
                              const std::string& cache_dir, bool enabled = true);

    std::string name() const override { return name_; }
    std::string url() const override { return url_; }
    bool enabled() const override { return enabled_; }
    void set_enabled(bool enabled) override { enabled_ = enabled; }

    std::optional<Object> find(const std::string& ns, const std::string& name) const override;
    std::optional<Object> find_by_ref(const ObjectRef& ref) const override;
    std::vector<Object> list_objects() const override;
    Collection<Object> search(const std::string& query) const override;
    Collection<Object> find_pattern(const std::string& pattern) const override;

    std::string type_name() const override { return "remote"; }

    bool sync() override;
    bool save() const override;
    bool load() override;

    // 获取并下载软件包 Artifact 到本地缓存目录，返回本地文件路径
    std::optional<std::string> fetch_artifact(const Object& obj, const std::string& dest_dir = "") override;

    const std::string& cache_dir() const { return cache_dir_; }

private:
    std::string name_;
    std::string url_;
    std::string cache_dir_;
    bool enabled_{true};
    std::vector<Object> objects_;
};

// RepositoryManager: 管理多个仓库
class RepositoryManager {
public:
    explicit RepositoryManager(const std::string& base_dir = "");

    // 添加/移除仓库
    void add_repository(std::unique_ptr<Repository> repo);
    void remove_repository(const std::string& name);

    // 启用/禁用
    bool enable_repository(const std::string& name);
    bool disable_repository(const std::string& name);

    // 查询
    std::vector<Repository*> repositories() const;
    std::vector<std::shared_ptr<Repository>> list() const;
    Repository* get_repository(const std::string& name) const;
    Repository* get(const std::string& name) const { return get_repository(name); }
    void add(std::shared_ptr<Repository> repo);
    void remove(const std::string& name) { remove_repository(name); }
    bool enable(const std::string& name) { return enable_repository(name); }
    bool disable(const std::string& name) { return disable_repository(name); }

    // 跨仓库查找
    std::optional<Object> find(const std::string& ref_or_ns, const std::string& name = "") const;
    std::optional<Object> find_by_ref(const ObjectRef& ref) const;
    Collection<Object> search(const std::string& query) const;
    Collection<Object> find_pattern(const std::string& pattern) const;

    void save_config() const;
    Collection<Object> list_all() const;
    Collection<Object> all_objects() const { return list_all(); }

    // 同步所有仓库
    bool sync_all();
    bool sync_repository(const std::string& name);

    // 获取特定对象所在仓库
    Repository* get_repository_for_object(const Object& obj) const;

private:
    std::string base_dir_;
    std::vector<std::shared_ptr<Repository>> repositories_;
};

} // namespace okrapm
