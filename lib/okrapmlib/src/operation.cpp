#include "okrapmlib/operation.h"
#include <sstream>

namespace okrapm {

Operation::Operation(OperationType type, Object target, Object old_version)
    : type_(type), target_(std::move(target)), old_version_(std::move(old_version)) {}

std::string Operation::description() const {
    std::string result;
    result += prefix();
    result += " ";
    result += target_.full_name();
    result += "@" + target_.version().to_string();

    if (type_ == OperationType::Update && old_version_.version() != Version{}) {
        result += " (from @" + old_version_.version().to_string() + ")";
    }

    return result;
}

char Operation::prefix() const {
    switch (type_) {
        case OperationType::Install: return '+';
        case OperationType::Remove:  return '-';
        case OperationType::Purge:   return '!';
        case OperationType::Update:  return '~';
        case OperationType::Upgrade: return '^';
        case OperationType::Sync:    return '=';
    }
    return '?';
}

std::string Operation::serialize() const {
    std::string result;
    switch (type_) {
        case OperationType::Install: result += "type=install\n"; break;
        case OperationType::Remove:  result += "type=remove\n"; break;
        case OperationType::Purge:   result += "type=purge\n"; break;
        case OperationType::Update:  result += "type=update\n"; break;
        case OperationType::Upgrade: result += "type=upgrade\n"; break;
        case OperationType::Sync:    result += "type=sync\n"; break;
    }
    result += "target_ns=" + target_.ns() + "\n";
    result += "target_name=" + target_.name() + "\n";
    result += "target_ver=" + target_.version().to_string() + "\n";
    if (type_ == OperationType::Update) {
        result += "old_ver=" + old_version_.version().to_string() + "\n";
    }
    return result;
}

std::optional<Operation> Operation::deserialize(const std::string& data) {
    Operation op;
    std::istringstream iss(data);
    std::string line;
    std::string ns, name, ver_str, old_ver_str;

    while (std::getline(iss, line)) {
        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;
        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        if (key == "type") {
            if (value == "install") op.type_ = OperationType::Install;
            else if (value == "remove") op.type_ = OperationType::Remove;
            else if (value == "purge") op.type_ = OperationType::Purge;
            else if (value == "update") op.type_ = OperationType::Update;
            else if (value == "upgrade") op.type_ = OperationType::Upgrade;
            else if (value == "sync") op.type_ = OperationType::Sync;
        } else if (key == "target_ns") ns = value;
        else if (key == "target_name") name = value;
        else if (key == "target_ver") ver_str = value;
        else if (key == "old_ver") old_ver_str = value;
    }

    if (name.empty()) return std::nullopt;

    auto ver = Version::parse(ver_str);
    op.target_ = Object(ns, name, ver ? *ver : Version{});

    if (!old_ver_str.empty()) {
        auto old_ver = Version::parse(old_ver_str);
        op.old_version_ = Object(ns, name, old_ver ? *old_ver : Version{});
    }

    return op;
}

} // namespace okrapm
