#pragma once

#include "version.h"
#include "object.h"
#include <string>
#include <optional>

namespace okrapm {

class ObjectRef {
public:
    ObjectRef() = default;

    static std::optional<ObjectRef> parse(const std::string& str);

    ObjectType type() const { return type_; }
    const std::string& ns() const { return ns_; }
    const std::string& name() const { return name_; }
    const std::optional<Version>& version() const { return version_; }
    std::string version_str() const { return !raw_version_.empty() ? raw_version_ : (version_ ? version_->to_string() : ""); }
    const std::string& artifact_path() const { return artifact_path_; }

    bool is_package() const { return type_ == ObjectType::Package; }
    bool is_group() const { return type_ == ObjectType::Group; }
    bool is_artifact() const { return type_ == ObjectType::Artifact; }
    bool is_system() const { return type_ == ObjectType::System; }

    std::string to_string() const;

    bool is_pattern() const { return is_pattern_; }
    bool matches(const Object& obj) const;
    static bool matches_pattern(const std::string& pattern, const std::string& str);

private:
    ObjectType type_{ObjectType::Package};
    std::string ns_;
    std::string name_;
    std::optional<Version> version_;
    std::string raw_version_;
    std::string artifact_path_;
    bool is_pattern_{false};
};

}
