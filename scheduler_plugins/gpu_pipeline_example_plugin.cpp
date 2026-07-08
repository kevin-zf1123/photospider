// Photospider scheduler plugin: GPU Pipeline Scheduler (Example)
// 编译为独立 dylib，可动态加载
// 注意：这是示例插件，类型名带 _example 后缀，避免与内置冲突

#include <string>

#include "kernel/scheduler/gpu_pipeline_scheduler.hpp"
#include "kernel/scheduler/scheduler_plugin_api.hpp"

// GPU Pipeline 调度器示例插件，使用带 _example 后缀的类型名
extern "C" {

int ps_scheduler_plugin_get_count() {
  return 2;  // gpu_pipeline_example 和 heterogeneous_example 两个别名
}

const char* ps_scheduler_plugin_get_name(int index) {
  switch (index) {
    case 0:
      return "gpu_pipeline_example";
    case 1:
      return "heterogeneous_example";
    default:
      return nullptr;
  }
}

const char* ps_scheduler_plugin_get_description(int index) {
  static const char* desc =
      "Example: Heterogeneous GPU/CPU pipeline scheduler. "
      "HP tasks prefer GPU (Metal) for throughput, "
      "RT tasks use CPU for low latency. "
      "Supports concurrent RT and HP scheduling with RT priority.";
  return (index == 0 || index == 1) ? desc : nullptr;
}

/**
 * @brief Creates an example GPU pipeline scheduler plugin instance.
 *
 * The example aliases use the active `GpuPipelineScheduler::Config` surface:
 * HP dispatch may prefer GPU work queues, while RT dispatch stays on the
 * scheduler's CPU RT queue through the scheduler's built-in queue topology
 * rather than through a deprecated config flag.
 *
 * @param type_name Scheduler type requested by the plugin loader.
 * @param num_workers Requested CPU worker count; zero preserves the scheduler's
 * automatic worker-count behavior.
 * @return A heap-allocated scheduler for supported aliases, or nullptr when
 * `type_name` is not provided by this plugin.
 * @throws std::bad_alloc if scheduler allocation fails.
 * @note Ownership transfers to the plugin loader, which must release the
 * instance through `ps_scheduler_plugin_destroy()`.
 */
ps::IScheduler* ps_scheduler_plugin_create(const char* type_name,
                                           unsigned int num_workers) {
  std::string name(type_name);
  if (name == "gpu_pipeline_example" || name == "heterogeneous_example") {
    ps::GpuPipelineScheduler::Config config;
    config.cpu_workers = num_workers;
    config.prefer_gpu_for_hp = true;
    return new ps::GpuPipelineScheduler(config);
  }
  return nullptr;
}

void ps_scheduler_plugin_destroy(ps::IScheduler* scheduler) {
  delete scheduler;
}

const char* ps_scheduler_plugin_get_version() {
  return "1.0.0";
}

}  // extern "C"
