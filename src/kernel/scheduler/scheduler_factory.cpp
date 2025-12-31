// Photospider kernel: Scheduler Factory implementation
// M3.4: 根据配置字符串创建对应的调度器实例

#include "kernel/scheduler/scheduler_factory.hpp"

#include "kernel/scheduler/cpu_work_stealing_scheduler.hpp"
#include "kernel/scheduler/serial_debug_scheduler.hpp"

namespace ps {

std::unique_ptr<IScheduler> SchedulerFactory::create(const std::string& type_name,
                                                     unsigned int num_workers) {
  if (type_name == "cpu_work_stealing") {
    return std::make_unique<CpuWorkStealingScheduler>(num_workers);
  } else if (type_name == "serial_debug") {
    return std::make_unique<SerialDebugScheduler>();
  }
  
  // 类型不支持
  return nullptr;
}

std::vector<std::string> SchedulerFactory::supported_types() {
  return {
    "cpu_work_stealing",
    "serial_debug"
  };
}

bool SchedulerFactory::is_supported(const std::string& type_name) {
  auto types = supported_types();
  return std::find(types.begin(), types.end(), type_name) != types.end();
}

std::string SchedulerFactory::description(const std::string& type_name) {
  if (type_name == "cpu_work_stealing") {
    return "Multi-threaded CPU scheduler with work stealing for load balancing. "
           "Optimal for parallel computation on multi-core systems.";
  } else if (type_name == "serial_debug") {
    return "Single-threaded serial scheduler for debugging. "
           "All tasks execute sequentially on the calling thread.";
  }
  return "Unknown scheduler type";
}

}  // namespace ps
