#pragma once

#include "object.h"
#include "object_ref.h"
#include <string>
#include <vector>

namespace okrapm {

enum class OperationType {
    Install,
    Remove,
    Purge,
    Update,
    Upgrade,
    Sync,
};

class Operation {
public:
    Operation() = default;
    Operation(OperationType type, Object target, Object old_version = {});

    OperationType type() const { return type_; }
    const Object& target() const { return target_; }
    const Object& old_version() const { return old_version_; }

    std::string description() const;
    std::string to_string() const { return description(); }

    char prefix() const;

    std::string serialize() const;
    static std::optional<Operation> deserialize(const std::string& data);

private:
    OperationType type_{OperationType::Install};
    Object target_;
    Object old_version_;
};

}
