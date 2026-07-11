/**
 * @file kernel_scheduler_facade.cpp
 * @brief Implements Kernel scheduler replacement and scheduler inspection
 * facades.
 */
#include <new>
#include <string>
#include <utility>

#include "runtime/kernel.hpp"
#include "scheduler/scheduler_factory.hpp"

namespace ps {

/** @copydoc Kernel::replace_scheduler */
bool Kernel::replace_scheduler(const std::string& name, ComputeIntent intent,
                               const std::string& type) {
  auto runtime_it = graphs_.find(name);
  if (runtime_it == graphs_.end()) {
    return false;
  }

  auto& runtime = *runtime_it->second;
  try {
    return runtime.graph_state()
        .submit([this, &runtime, intent, type](GraphModel&) {
          auto scheduler =
              SchedulerFactory::create(type, scheduler_config_.worker_count);
          if (!scheduler) {
            return false;
          }
          runtime.replace_scheduler(intent, std::move(scheduler));
          return true;
        })
        .get();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return false;
  }
}

/** @copydoc Kernel::get_scheduler_info */
std::optional<std::pair<std::string, std::string>> Kernel::get_scheduler_info(
    const std::string& name, ComputeIntent intent) {
  auto runtime_it = graphs_.find(name);
  if (runtime_it == graphs_.end()) {
    return std::nullopt;
  }

  auto& runtime = *runtime_it->second;
  try {
    return runtime.graph_state()
        .submit([&runtime, intent](GraphModel&)
                    -> std::optional<std::pair<std::string, std::string>> {
          const IScheduler* scheduler = runtime.get_scheduler(intent);
          if (!scheduler) {
            return std::nullopt;
          }
          return std::make_pair(scheduler->name(), scheduler->get_stats());
        })
        .get();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace ps
