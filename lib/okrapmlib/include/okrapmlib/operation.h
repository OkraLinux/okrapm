#pragma once

#include "object.h"
#include "object_ref.h"
#include <string>
#include <vector>

namespace okrapm {

// 操作类型
enum class OperationType {
    Install,    // 安装
    Remove,     // 删除
    Purge,      // 清除 (删除包+配置)
    Update,     // 更新
    Upgrade,    // 系统级升级
    Sync,       // 同步
};

// Operation: 事务中的单个操作
// 每个操作描述对系统状态的一个原子变更
class Operation {
public:
    Operation() = default;
    Operation(OperationType type, Object target, Object old_version = {});

    OperationType type() const { return type_; }
    const Object& target() const { return target_; }
    const Object& old_version() const { return old_version_; }

    // 操作描述文本
    std::string description() const;
    std::string to_string() const { return description(); }

    // 操作前缀符号: + 安装, - 删除, ~ 更新
    char prefix() const;

    // 序列化
    std::string serialize() const;
    static std::optional<Operation> deserialize(const std::string& data);

private:
    OperationType type_{OperationType::Install};
    Object target_;       // 目标对象
    Object old_version_;  // 旧版本 (更新操作时使用)
};

} // namespace okrapm
