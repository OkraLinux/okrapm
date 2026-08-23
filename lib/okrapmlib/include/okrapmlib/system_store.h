#pragma once

#include "object.h"
#include "collection.h"
#include "transaction.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace okrapm {

// SystemState: 系统状态
// 维护连续系统状态: S(t) -> S(t+1) -> S(t+2) -> ...
struct SystemState {
    uint64_t id{0};        // 状态 ID
    std::string label;     // 状态标签 (如 "2026-08-23")
    std::vector<Object> objects;  // 已安装对象
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};

    std::string to_string() const;
};

// SystemStore: 系统状态存储
// 维护已安装对象数据库和系统状态
class SystemStore {
public:
    explicit SystemStore(const std::string& store_path);

    // 加载/保存
    bool load();
    bool save() const;

    // 已安装对象查询
    std::optional<Object> find(const std::string& ns, const std::string& name) const;
    std::vector<Object> list_installed() const { return installed_; }
    Collection<Object> collection() const;
    bool is_installed(const std::string& ns, const std::string& name) const;

    // 修改操作
    void install(const Object& obj);
    void remove(const std::string& ns, const std::string& name);
    void update(const Object& obj);  // 更新已安装对象的版本

    // 获取反向依赖
    std::vector<Object> reverse_dependencies(const std::string& ns, const std::string& name) const;

    // 当前系统状态
    SystemState current_state() const;
    void set_current_state(const SystemState& state) { current_ = state; }

    // 获取下一个状态 ID
    uint64_t next_state_id() const { return next_state_id_; }
    void advance_state_id() { ++next_state_id_; }

private:
    std::string store_path_;
    std::vector<Object> installed_;
    uint64_t next_state_id_{1};
    SystemState current_;
};

} // namespace okrapm
