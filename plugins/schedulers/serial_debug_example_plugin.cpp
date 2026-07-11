// Photospider scheduler plugin: Serial Debug Scheduler (Example)
// 编译为独立 dylib，可动态加载
// 注意：这是示例插件，类型名为 serial_debug_example，避免与内置冲突

#include <string>

#include "kernel/scheduler/scheduler_plugin_api.hpp"
#include "scheduler/serial_debug_scheduler.hpp"

// 使用宏简化单一调度器插件的实现
PS_IMPLEMENT_SINGLE_SCHEDULER_PLUGIN(
    "serial_debug_example",
    "Example: Single-threaded serial scheduler for debugging. "
    "All tasks execute sequentially on the calling thread.",
    new ps::SerialDebugScheduler())
