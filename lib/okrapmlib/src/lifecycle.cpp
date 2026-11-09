#include "okrapmlib/lifecycle.h"

namespace okrapm {

namespace {

std::shared_ptr<LifecycleRunner>& runner_slot()
{
    static std::shared_ptr<LifecycleRunner> runner;
    return runner;
}

}

void set_lifecycle_runner(std::shared_ptr<LifecycleRunner> runner)
{
    runner_slot() = std::move(runner);
}

std::shared_ptr<LifecycleRunner> lifecycle_runner()
{
    return runner_slot();
}

}
