/**
 * @file scheduler_factory.cpp
 * @brief Implements pure scheduler planning and concrete construction.
 */

#include "scheduler/scheduler_factory.hpp"

#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/core/graph_error.hpp"
#include "scheduler/cpu_work_stealing_scheduler.hpp"
#include "scheduler/gpu_pipeline_scheduler.hpp"
#include "scheduler/scheduler_plugin_loader.hpp"
#include "scheduler/scheduler_reservation_owner.hpp"
#include "scheduler/scheduler_worker_budget.hpp"
#include "scheduler/serial_debug_scheduler.hpp"

namespace ps {

/** @copydoc SchedulerPlan::SchedulerPlan */
SchedulerPlan::SchedulerPlan(std::string type_name, Kind kind,
                             unsigned int worker_grant,
                             unsigned int reservation_slots) noexcept
    : type_name_(std::move(type_name)),
      kind_(kind),
      worker_grant_(worker_grant),
      reservation_slots_(reservation_slots) {}  // NOLINT

/** @copydoc SchedulerPlan::type_name */
const std::string& SchedulerPlan::type_name() const noexcept {
  return type_name_;
}

/** @copydoc SchedulerPlan::worker_grant */
unsigned int SchedulerPlan::worker_grant() const noexcept {
  return worker_grant_;
}

/** @copydoc SchedulerPlan::reservation_slots */
unsigned int SchedulerPlan::reservation_slots() const noexcept {
  return reservation_slots_;
}

/** @copydoc SchedulerFactory::plan */
std::optional<SchedulerPlan> SchedulerFactory::plan(
    const std::string& type_name, unsigned int num_workers) {
  return plan_for_hardware(type_name, num_workers,
                           std::thread::hardware_concurrency());
}

/** @copydoc SchedulerFactory::plan_for_hardware */
std::optional<SchedulerPlan> SchedulerFactory::plan_for_hardware(
    const std::string& type_name, unsigned int num_workers,
    unsigned int detected_hardware_workers) {
  SchedulerPlan::Kind kind;
  if (type_name == "serial_debug") {
    kind = SchedulerPlan::Kind::Serial;
  } else if (type_name == "cpu_work_stealing") {
    kind = SchedulerPlan::Kind::Cpu;
  } else if (type_name == "gpu_pipeline" || type_name == "heterogeneous") {
    kind = SchedulerPlan::Kind::Gpu;
  } else if (SchedulerPluginLoader::instance().is_registered(type_name)) {
    kind = SchedulerPlan::Kind::Plugin;
  } else {
    return std::nullopt;
  }

  const unsigned int resolved_workers =
      resolve_scheduler_worker_count(num_workers, detected_hardware_workers);
  switch (kind) {
    case SchedulerPlan::Kind::Serial:
      return SchedulerPlan(type_name, kind, 0U, 0U);
    case SchedulerPlan::Kind::Cpu:
    case SchedulerPlan::Kind::Plugin:
      return SchedulerPlan(type_name, kind, resolved_workers, resolved_workers);
    case SchedulerPlan::Kind::Gpu: {
      const unsigned int reservation_slots =
          checked_add_scheduler_worker_slots(resolved_workers, 1U);
      if (reservation_slots > kGpuSchedulerWorkerInstanceMax) {
        throw std::overflow_error(
            "GPU scheduler worker plan exceeds its instance ceiling");
      }
      return SchedulerPlan(type_name, kind, resolved_workers,
                           reservation_slots);
    }
  }
  return std::nullopt;
}

/** @copydoc SchedulerFactory::create */
std::unique_ptr<IScheduler> SchedulerFactory::create(
    const SchedulerPlan& plan, SchedulerWorkerBudget::Reservation reservation) {
  if (!reservation.active() || reservation.slots() != plan.reservation_slots_) {
    throw std::invalid_argument(
        "scheduler construction requires an exact active reservation");
  }

  std::unique_ptr<IScheduler> scheduler;
  switch (plan.kind_) {
    case SchedulerPlan::Kind::Serial:
      scheduler = std::make_unique<SerialDebugScheduler>();
      break;
    case SchedulerPlan::Kind::Cpu:
      scheduler =
          std::make_unique<CpuWorkStealingScheduler>(plan.worker_grant_);
      break;
    case SchedulerPlan::Kind::Gpu: {
      GpuPipelineScheduler::Config config;
      config.cpu_workers = plan.worker_grant_;
      config.gpu_workers = 1U;
      config.prefer_gpu_for_hp = true;
      scheduler = std::make_unique<GpuPipelineScheduler>(config);
      break;
    }
    case SchedulerPlan::Kind::Plugin:
      scheduler = SchedulerPluginLoader::instance().create(plan.type_name_,
                                                           plan.worker_grant_);
      break;
  }
  return make_reservation_owned_scheduler(std::move(scheduler),
                                          std::move(reservation));
}

/** @copydoc SchedulerFactory::create */
std::unique_ptr<IScheduler> SchedulerFactory::create(
    const std::string& type_name, unsigned int num_workers) {
  std::optional<SchedulerPlan> scheduler_plan = plan(type_name, num_workers);
  if (!scheduler_plan.has_value()) {
    return nullptr;
  }
  auto reservation = SchedulerWorkerBudget::process().try_reserve(
      scheduler_plan->reservation_slots());
  if (!reservation.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "process scheduler worker budget is exhausted");
  }
  return create(*scheduler_plan, std::move(*reservation));
}

/** @copydoc SchedulerFactory::supported_types */
std::vector<std::string> SchedulerFactory::supported_types() {
  return {"cpu_work_stealing", "serial_debug", "gpu_pipeline", "heterogeneous"};
}

/** @copydoc SchedulerFactory::is_supported */
bool SchedulerFactory::is_supported(const std::string& type_name) {
  if (type_name == "cpu_work_stealing" || type_name == "serial_debug" ||
      type_name == "gpu_pipeline" || type_name == "heterogeneous") {
    return true;
  }
  return SchedulerPluginLoader::instance().is_registered(type_name);
}

/** @copydoc SchedulerFactory::description */
std::string SchedulerFactory::description(const std::string& type_name) {
  if (type_name == "cpu_work_stealing") {
    return "Multi-threaded CPU scheduler with work stealing for load "
           "balancing. Optimal for parallel computation on multi-core "
           "systems.";
  }
  if (type_name == "serial_debug") {
    return "Single-threaded serial scheduler for debugging. All tasks execute "
           "sequentially on the calling thread.";
  }
  if (type_name == "gpu_pipeline" || type_name == "heterogeneous") {
    return "Heterogeneous GPU/CPU pipeline scheduler. Normal-priority HP tasks "
           "may use a GPU queue when Metal is available, RT tasks use a "
           "high-priority CPU queue, and RT scheduling has priority.";
  }

  return SchedulerPluginLoader::instance().get_description(type_name);
}

}  // namespace ps
