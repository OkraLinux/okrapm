#include "okrapm-opsis-bridge/bridge.h"
#include "okrapmlib/lifecycle.h"
#include "opsis/interpreter.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace okrapm {

namespace {

const char* const kProgramNames[] = {
    "package.opsis", "lifecycle.opsis", "install.opsis", "update.opsis", "upgrade.opsis", "remove.opsis"
};

class OpsisLifecycleRunner : public LifecycleRunner {
public:
    std::string host_architecture() const override
    {
        return Opsis::HostArchitecture();
    }

    bool has_program(const std::string& package_dir) const override
    {
        for (const char* name : kProgramNames) {
            if (fs::is_regular_file(fs::path(package_dir) / name)) return true;
            if (fs::is_regular_file(fs::path(package_dir) / "scripts" / name)) return true;
        }
        return false;
    }

    bool collect_program(const std::string& package_dir, const std::string& dest_dir) const override
    {
        std::error_code ec;
        const fs::path staging(package_dir);
        const fs::path incoming(dest_dir);
        fs::create_directories(incoming, ec);
        if (ec) return false;
        for (const char* name : kProgramNames) {
            for (const fs::path& rel : {fs::path(name), fs::path("scripts") / name}) {
                const fs::path src = staging / rel;
                if (!fs::is_regular_file(src)) continue;
                const fs::path dest = incoming / rel;
                fs::create_directories(dest.parent_path(), ec);
                if (ec) return false;
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
                if (ec) return false;
            }
        }
        if (fs::is_directory(staging / "lib")) {
            fs::copy(staging / "lib", incoming / "lib", fs::copy_options::recursive, ec);
            if (ec) return false;
        }
        return true;
    }

    int run(const std::string& package_dir, const std::string& entry,
            const std::string& sysroot, const LifecycleScene& scene) override
    {
        Opsis::PackageScene opsis_scene;
        opsis_scene.Namespace = scene.ns;
        opsis_scene.Name = scene.name;
        opsis_scene.Version = scene.version;
        opsis_scene.OldVersion = scene.old_version;
        opsis_scene.NewVersion = scene.new_version;
        opsis_scene.Architecture = scene.architecture;
        opsis_scene.Reason = scene.reason;
        return Opsis::RunLifecycleScript(package_dir, entry, sysroot, false, opsis_scene);
    }
};

}

void install_opsis_lifecycle_runner()
{
    set_lifecycle_runner(std::make_shared<OpsisLifecycleRunner>());
}

}
