#include "okrapmlib/object_ref.h"
#include <algorithm>

namespace okrapm {

std::optional<ObjectRef> ObjectRef::parse(const std::string& str) {
    if (str.empty()) return std::nullopt;

    ObjectRef ref;
    std::string input = str;

    // Check for artifact path (.oaa or .okra or starts with ./)
    if ((input.size() > 4 && input.substr(input.size() - 4) == ".oaa") ||
        (input.size() > 5 && input.substr(input.size() - 5) == ".okra")) {
        ref.type_ = ObjectType::Artifact;
        ref.artifact_path_ = input;
        auto slash = input.rfind('/');
        std::string fname = (slash == std::string::npos) ? input : input.substr(slash + 1);
        ref.ns_ = "local";
        if (fname.size() > 5 && fname.substr(fname.size() - 5) == ".okra") {
            ref.name_ = fname.substr(0, fname.size() - 5);
        } else {
            ref.name_ = fname.substr(0, fname.size() - 4);
        }
        return ref;
    }

    // Check for group marker '#'
    if (input.front() == '#') {
        ref.type_ = ObjectType::Group;
        input = input.substr(1);
    }

    // Check for version delimiter '@'
    auto at_pos = input.find('@');
    std::string name_part = input;
    if (at_pos != std::string::npos) {
        name_part = input.substr(0, at_pos);
        std::string ver_str = input.substr(at_pos + 1);
        auto ver = Version::parse(ver_str);
        if (!ver) return std::nullopt;
        ref.version_ = ver;
        ref.raw_version_ = ver_str;
    }

    // Check for wildcard pattern (e.g., "gnu.*")
    if (name_part.find('*') != std::string::npos) {
        ref.is_pattern_ = true;
    }

    // Parse namespace.name
    auto dot_pos = name_part.find('.');
    if (dot_pos != std::string::npos) {
        if (dot_pos == 0 || dot_pos == name_part.size() - 1 ||
            name_part.find('.', dot_pos + 1) != std::string::npos) {
            return std::nullopt;
        }
        ref.ns_ = name_part.substr(0, dot_pos);
        ref.name_ = name_part.substr(dot_pos + 1);
    } else {
        // No namespace specified, treat entire string as name with default namespace
        ref.ns_ = "";
        ref.name_ = name_part;
    }

    if (ref.name_.empty() && !ref.is_pattern_) return std::nullopt;

    // Distinguish system objects (e.g., okra.system*)
    if (ref.type_ == ObjectType::Package && ref.ns_ == "okra" && ref.name_.rfind("system", 0) == 0) {
        ref.type_ = ObjectType::System;
    }

    return ref;
}

std::string ObjectRef::to_string() const {
    if (type_ == ObjectType::Artifact) {
        return artifact_path_;
    }

    std::string result;
    if (type_ == ObjectType::Group) {
        result += "#";
    }
    if (!ns_.empty()) {
        result += ns_ + ".";
    }
    result += name_;
    if (version_.has_value()) {
        result += "@" + version_->to_string();
    }
    return result;
}

bool ObjectRef::matches(const Object& obj) const {
    // Type match
    if (type_ != obj.type()) {
        // If ref is Package but target is System (or vice versa), allow if names match
        if (!((type_ == ObjectType::Package && obj.type() == ObjectType::System) ||
              (type_ == ObjectType::System && obj.type() == ObjectType::Package))) {
            return false;
        }
    }

    // Pattern matching (e.g., ns="gnu", name="*")
    if (is_pattern_) {
        if (!ns_.empty() && ns_ != "*" && ns_ != obj.ns()) {
            return false;
        }
        if (name_ == "*") {
            return true;
        }
        if (name_.back() == '*') {
            std::string prefix = name_.substr(0, name_.size() - 1);
            return obj.name().rfind(prefix, 0) == 0;
        }
    }

    // Exact match
    if (!ns_.empty() && ns_ != obj.ns()) {
        return false;
    }
    if (name_ != obj.name()) {
        return false;
    }

    // Version match if specified
    if (version_.has_value()) {
        if (obj.version() != *version_) {
            return false;
        }
    }

    return true;
}

bool ObjectRef::matches_pattern(const std::string& pattern, const std::string& str) {
    if (pattern == "*" || pattern.empty()) return true;
    if (pattern.back() == '*') {
        std::string prefix = pattern.substr(0, pattern.size() - 1);
        return str.rfind(prefix, 0) == 0;
    }
    if (pattern.front() == '*') {
        std::string suffix = pattern.substr(1);
        if (str.size() < suffix.size()) return false;
        return str.rfind(suffix) == (str.size() - suffix.size());
    }
    return pattern == str;
}

} // namespace okrapm
