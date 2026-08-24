#include "okrapmlib/object.h"
#include <sstream>

namespace okrapm {

Object::Object(std::string ns, std::string name, Version version, ObjectType type)
    : ns_(std::move(ns)), name_(std::move(name)), version_(std::move(version)), type_(type) {}

std::string Object::full_name() const {
    if (type_ == ObjectType::Group) {
        return "#" + ns_ + "." + name_;
    }
    return ns_ + "." + name_;
}

std::string Object::ref_string() const {
    std::string result = full_name();
    std::string ver_str = version_.to_string();
    if (ver_str != "0.0.0") {
        result += "@" + ver_str;
    }
    return result;
}

char Object::type_prefix(ObjectType type) {
    switch (type) {
        case ObjectType::Package: return ' ';
        case ObjectType::Group: return '#';
        case ObjectType::System: return '$';
        case ObjectType::Artifact: return '@';
        case ObjectType::Repository: return '&';
    }
    return ' ';
}

std::string Object::type_name(ObjectType type) {
    switch (type) {
        case ObjectType::Package: return "package";
        case ObjectType::Group: return "group";
        case ObjectType::System: return "system";
        case ObjectType::Artifact: return "artifact";
        case ObjectType::Repository: return "repository";
    }
    return "unknown";
}

std::string Object::serialize() const {
    std::ostringstream oss;
    oss << "ns=" << ns_ << "\t"
        << "name=" << name_ << "\t"
        << "version=" << version_.to_string() << "\t"
        << "type=" << type_name(type_) << "\t"
        << "repo=" << repository_ << "\t"
        << "desc=" << description_ << "\t"
        << "dsize=" << download_size_ << "\t"
        << "isize=" << installed_size_ << "\t";

    oss << "deps=";
    for (size_t i = 0; i < dependencies_.size(); ++i) {
        if (i > 0) oss << ",";
        oss << dependencies_[i];
    }
    oss << "\t";

    oss << "files=";
    for (size_t i = 0; i < files_.size(); ++i) {
        if (i > 0) oss << ",";
        oss << files_[i];
    }
    return oss.str();
}

std::optional<Object> Object::deserialize(const std::string& data) {
    if (data.empty()) return std::nullopt;
    Object obj;
    bool has_name = false;

    auto process_kv = [&](const std::string& kv) {
        auto eq_pos = kv.find('=');
        if (eq_pos == std::string::npos) return;
        std::string key = kv.substr(0, eq_pos);
        std::string value = kv.substr(eq_pos + 1);

        if (key == "ns") {
            obj.ns_ = value;
        } else if (key == "name") {
            obj.name_ = value;
            has_name = true;
        } else if (key == "version") {
            auto ver = Version::parse(value);
            if (ver) obj.version_ = *ver;
        } else if (key == "type") {
            if (value == "group") obj.type_ = ObjectType::Group;
            else if (value == "system") obj.type_ = ObjectType::System;
            else if (value == "artifact") obj.type_ = ObjectType::Artifact;
            else if (value == "repository") obj.type_ = ObjectType::Repository;
            else obj.type_ = ObjectType::Package;
        } else if (key == "repo") {
            obj.repository_ = value;
        } else if (key == "desc") {
            obj.description_ = value;
        } else if (key == "dsize") {
            try { obj.download_size_ = std::stoull(value); } catch (...) {}
        } else if (key == "isize") {
            try { obj.installed_size_ = std::stoull(value); } catch (...) {}
        } else if (key == "deps") {
            if (!value.empty()) {
                std::istringstream deps_iss(value);
                std::string dep;
                while (std::getline(deps_iss, dep, ',')) {
                    if (!dep.empty()) obj.dependencies_.push_back(dep);
                }
            }
        } else if (key == "files") {
            if (!value.empty()) {
                std::istringstream files_iss(value);
                std::string file;
                while (std::getline(files_iss, file, ',')) {
                    if (!file.empty()) obj.files_.push_back(file);
                }
            }
        }
    };

    std::string token;
    for (char c : data) {
        if (c == '\t' || c == '\n' || c == '\r') {
            if (!token.empty()) {
                process_kv(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }
    if (!token.empty()) process_kv(token);

    if (!has_name) return std::nullopt;
    if (obj.ns_.empty()) obj.ns_ = "okra"; // default namespace
    return obj;
}

Artifact::Artifact(std::string path)
    : Object("local", "artifact", {}, ObjectType::Artifact), artifact_path_(std::move(path)) {
    // deduce name from file path
    auto slash = artifact_path_.rfind('/');
    std::string filename = (slash == std::string::npos) ? artifact_path_ : artifact_path_.substr(slash + 1);
    if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".okra") {
        name_ = filename.substr(0, filename.size() - 5);
    } else {
        auto dot = filename.rfind(".oaa");
        if (dot != std::string::npos) {
            name_ = filename.substr(0, dot);
        } else {
            name_ = filename;
        }
    }
}

std::string Artifact::serialize() const {
    std::string result = Object::serialize();
    result += "path=" + artifact_path_ + "\n";
    return result;
}

std::optional<Artifact> Artifact::deserialize(const std::string& data) {
    auto base_obj = Object::deserialize(data);
    if (!base_obj) return std::nullopt;

    Artifact art;
    art.ns_ = base_obj->ns();
    art.name_ = base_obj->name();
    art.version_ = base_obj->version();
    art.type_ = ObjectType::Artifact;
    art.description_ = base_obj->description();
    art.dependencies_ = base_obj->dependencies();
    art.files_ = base_obj->files();

    std::istringstream iss(data);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("path=", 0) == 0) {
            art.artifact_path_ = line.substr(5);
        }
    }
    return art;
}

} // namespace okrapm
