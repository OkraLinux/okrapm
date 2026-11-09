#pragma once

#include "object.h"
#include "collection.h"
#include "transaction.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace okrapm {

struct SystemState {
    uint64_t id{0};
    std::string label;
    std::vector<Object> objects;
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};

    std::string to_string() const;
};

class SystemStore {
public:
    explicit SystemStore(const std::string& store_path);

    bool load();
    bool save() const;

    std::optional<Object> find(const std::string& ns, const std::string& name) const;
    std::vector<Object> list_installed() const { return installed_; }
    Collection<Object> collection() const;
    bool is_installed(const std::string& ns, const std::string& name) const;

    void install(const Object& obj);
    void remove(const std::string& ns, const std::string& name);
    void update(const Object& obj);

    std::vector<Object> reverse_dependencies(const std::string& ns, const std::string& name) const;

    SystemState current_state() const;
    void set_current_state(const SystemState& state) { current_ = state; }

    uint64_t next_state_id() const { return next_state_id_; }
    void advance_state_id() { ++next_state_id_; }

private:
    std::string store_path_;
    std::vector<Object> installed_;
    uint64_t next_state_id_{1};
    SystemState current_;
};

}
