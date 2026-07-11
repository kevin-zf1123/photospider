/**
 * @file kernel_scheduler_facade.cpp
 * @brief Implements Kernel scheduler replacement and scheduler inspection
 * facades.
 */
#include <string>
#include <utility>

#include "runtime/kernel.hpp"
#include "scheduler/scheduler_factory.hpp"

namespace ps {

bool Kernel::replace_scheduler(const std::string& name, ComputeIntent intent,
                               const std::string& type) {
  return with_runtime(name,
                      [this, intent, type](GraphRuntime& runtime) {
                        auto scheduler = SchedulerFactory::create(
                            type, scheduler_config_.worker_count);
                        if (!scheduler) {
                          return false;
                        }
                        runtime.replace_scheduler(intent, std::move(scheduler));
                        return true;
                      })
      .value_or(false);
}

std::optional<std::pair<std::string, std::string>> Kernel::get_scheduler_info(
    const std::string& name, ComputeIntent intent) const {
  auto result = with_runtime(
      name,
      [intent](const GraphRuntime& runtime)
          -> std::optional<std::pair<std::string, std::string>> {
        const IScheduler* scheduler = runtime.get_scheduler(intent);
        if (!scheduler) {
          return std::nullopt;
        }
        return std::make_pair(scheduler->name(), scheduler->get_stats());
      });
  if (!result) {
    return std::nullopt;
  }
  return *result;
}

}  // namespace ps
