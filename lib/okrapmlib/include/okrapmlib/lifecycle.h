#pragma once

#include <memory>
#include <string>

namespace okrapm {

struct LifecycleScene {
    std::string ns;
    std::string name;
    std::string version;
    std::string old_version;
    std::string new_version;
    std::string architecture;
    std::string reason;
};

class LifecycleRunner {
public:
    virtual ~LifecycleRunner() = default;
    virtual std::string host_architecture() const = 0;
    virtual bool has_program(const std::string& package_dir) const = 0;
    virtual bool collect_program(const std::string& package_dir, const std::string& dest_dir) const = 0;
    virtual int run(const std::string& package_dir, const std::string& entry,
                    const std::string& sysroot, const LifecycleScene& scene) = 0;
};

void set_lifecycle_runner(std::shared_ptr<LifecycleRunner> runner);
std::shared_ptr<LifecycleRunner> lifecycle_runner();

}
