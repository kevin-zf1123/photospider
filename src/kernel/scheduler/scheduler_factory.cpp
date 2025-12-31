// Photospider kernel: Scheduler Factory implementation
// M3.4: 根据配置字符串创建对应的调度器实例
// M3.5: 支持从插件动态创建调度器

#include "kernel/scheduler/scheduler_factory.hpp"

#include "kernel/scheduler/cpu_work_stealing_scheduler.hpp"
#include "kernel/scheduler/gpu_pipeline_scheduler.hpp"
#include "kernel/scheduler/scheduler_plugin_loader.hpp"
#include "kernel/scheduler/serial_debug_scheduler.hpp"

namespace ps {

std::unique_ptr<IScheduler> SchedulerFactory::create(const std::string& type_name,
                                                     unsigned int num_workers) {
  // 先尝试内置类型
  if (type_name == "cpu_work_stealing") {
    return std::make_unique<CpuWorkStealingScheduler>(num_workers);
  } else if (type_name == "serial_debug") {
    return std::make_unique<SerialDebugScheduler>();
  } else if (type_name == "gpu_pipeline") {
    GpuPipelineScheduler::Config config;
    config.cpu_workers = num_workers;
    config.prefer_gpu_for_hp = true;
    config.force_cpu_for_rt = true;
    return std::make_unique<GpuPipelineScheduler>(config);
  } else if (type_name == "heterogeneous") {
    // 别名：异构调度器
    GpuPipelineScheduler::Config config;
    config.cpu_workers = num_workers;
    config.prefer_gpu_for_hp = true;
    config.force_cpu_for_rt = true;
    return std::make_unique<GpuPipelineScheduler>(config);
  }
  
  // 尝试从插件加载器创建
  auto& loader = SchedulerPluginLoader::instance();
  if (loader.is_registered(type_name)) {
    return loader.create(type_name, num_workers);
  }
  
  // 类型不支持
  return nullptr;
}

std::vector<std::string> SchedulerFactory::supported_types() {
  return {
    "cpu_work_stealing",
    "serial_debug",
    "gpu_pipeline",
    "heterogeneous"
  };
}

bool SchedulerFactory::is_supported(const std::string& type_name) {
  auto types = supported_types();
  if (std::find(types.begin(), types.end(), type_name) != types.end()) {
    return true;
  }
  // 也检查插件
  auto& loader = SchedulerPluginLoader::instance();
  return loader.is_registered(type_name);
}

std::string SchedulerFactory::description(const std::string& type_name) {
  if (type_name == "cpu_work_stealing") {
    return "Multi-threaded CPU scheduler with work stealing for load balancing. "
           "Optimal for parallel computation on multi-core systems.";
  } else if (type_name == "serial_debug") {
    return "Single-threaded serial scheduler for debugging. "
           "All tasks execute sequentially on the calling thread.";
  } else if (type_name == "gpu_pipeline" || type_name == "heterogeneous") {
    return "Heterogeneous GPU/CPU pipeline scheduler. "
           "HP tasks prefer GPU (Metal) for throughput, "
           "RT tasks use CPU for low latency. "
           "Supports concurrent RT and HP scheduling with RT priority.";
  }
  
  // 检查插件描述
  auto& loader = SchedulerPluginLoader::instance();
  auto desc = loader.get_description(type_name);
  if (!desc.empty()) {
    return desc;
  }
  
  return "Unknown scheduler type";
}

}  // namespace ps
