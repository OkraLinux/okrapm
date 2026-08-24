#include "okrapmlib/system_store.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <filesystem>

namespace okrapm {

std::string SystemState::to_string() const {
    std::ostringstream oss;
    oss << "#" << id << "  " << (label.empty() ? "State " + std::to_string(id) : label)
        << " (" << objects.size() << " objects)";
    return oss.str();
}

SystemStore::SystemStore(const std::string& store_path)
    : store_path_(store_path) {
    load();
}

bool SystemStore::load() {
    installed_.clear();
    std::ifstream ifs(store_path_);
    if (!ifs.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.rfind("STATE_ID:", 0) == 0) {
            try {
                next_state_id_ = std::stoull(line.substr(9));
            } catch (...) {}
            continue;
        }
        auto obj = Object::deserialize(line);
        if (obj) {
            installed_.push_back(*obj);
        } else {
            // 兼容 okpm 格式行: name version path
            std::istringstream iss(line);
            std::string name, ver_str, path;
            if (iss >> name >> ver_str >> path) {
                auto ver = Version::parse(ver_str);
                Object okpm_obj("okra", name, ver ? *ver : Version(1, 0, 0), ObjectType::Package);
                okpm_obj.set_repository("okpm-legacy");
                installed_.push_back(okpm_obj);
            }
        }
    }
    return true;
}

bool SystemStore::save() const {
    std::ofstream ofs(store_path_);
    if (!ofs.is_open()) {
        return false;
    }

    ofs << "# Lunar System Store\n";
    ofs << "STATE_ID:" << next_state_id_ << "\n";
    for (const auto& obj : installed_) {
        ofs << obj.serialize() << "\n";
    }
    return true;
}

std::optional<Object> SystemStore::find(const std::string& ns, const std::string& name) const {
    for (const auto& obj : installed_) {
        if (obj.ns() == ns && obj.name() == name) {
            return obj;
        }
    }
    return std::nullopt;
}

Collection<Object> SystemStore::collection() const {
    return Collection<Object>(installed_);
}

bool SystemStore::is_installed(const std::string& ns, const std::string& name) const {
    return find(ns, name).has_value();
}

void SystemStore::install(const Object& obj) {
    for (auto& item : installed_) {
        if (item.ns() == obj.ns() && item.name() == obj.name()) {
            item = obj;
            return;
        }
    }
    installed_.push_back(obj);
}

void SystemStore::remove(const std::string& ns, const std::string& name) {
    installed_.erase(
        std::remove_if(installed_.begin(), installed_.end(),
                       [&](const Object& obj) {
                           return obj.ns() == ns && obj.name() == name;
                       }),
        installed_.end()
    );
}

void SystemStore::update(const Object& obj) {
    install(obj);
}

std::vector<Object> SystemStore::reverse_dependencies(const std::string& ns, const std::string& name) const {
    std::string target = ns + "." + name;
    std::vector<Object> result;
    for (const auto& obj : installed_) {
        for (const auto& dep : obj.dependencies()) {
            std::string dep_base = dep.substr(0, dep.find('@'));
            if (dep_base == target || dep_base == name) {
                result.push_back(obj);
                break;
            }
        }
    }
    return result;
}

SystemState SystemStore::current_state() const {
    SystemState st;
    st.id = next_state_id_;
    st.label = "System #" + std::to_string(st.id);
    st.objects = installed_;
    st.timestamp = std::chrono::system_clock::now();
    return st;
}

} // namespace okrapm
