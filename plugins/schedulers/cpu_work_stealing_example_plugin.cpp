// Photospider scheduler plugin: CPU Work-Stealing Scheduler (Example)
// 编译为独立 dylib，可动态加载
// 注意：这是示例插件，类型名为 cpu_work_stealing_example，避免与内置冲突

#include <string>

#include "kernel/scheduler/scheduler_plugin_api.hpp"
#include "scheduler/cpu_work_stealing_scheduler.hpp"

// 使用宏简化单一调度器插件的实现
PS_IMPLEMENT_SINGLE_SCHEDULER_PLUGIN(
    "cpu_work_stealing_example",
    "Example: Multi-threaded CPU scheduler with work stealing for load "
    "balancing. "
    "Optimal for parallel computation on multi-core systems.",
    new ps::CpuWorkStealingScheduler(num_workers))
