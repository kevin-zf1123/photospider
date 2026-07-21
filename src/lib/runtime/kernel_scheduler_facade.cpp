/**
 * @file kernel_scheduler_facade.cpp
 * @brief Implements Kernel scheduler replacement and scheduler inspection
 * facades.
 */
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "compute/execution_service.hpp"
#include "photospider/core/graph_error.hpp"
#include "runtime/kernel.hpp"
#include "scheduler/scheduler_factory.hpp"

namespace ps {
namespace {

/**
 * @brief Internal result of one serialized scheduler replacement attempt.
 *
 * @throws Nothing for value construction and comparison.
 * @note The outcome distinguishes legacy-owner resource exhaustion from
 * handled rejection. Candidate/plugin GraphError and
 * ordinary lifecycle failures remain folded into `Rejected` by the outer
 * Kernel boundary.
 */
enum class SchedulerReplacementOutcome {
  /** @brief Candidate published and the displaced owner cleanup completed. */
  Success,
  /** @brief Type, factory, or handled candidate lifecycle rejected the call. */
  Rejected,
  /** @brief A valid candidate plan could not reserve transient capacity. */
  ResourceExhausted,
};

}  // namespace

/** @copydoc Kernel::replace_scheduler */
bool Kernel::replace_scheduler(const std::string& name, ComputeIntent intent,
                               const std::string& type) {
  auto runtime_it = graphs_.find(name);
  if (runtime_it == graphs_.end()) {
    return false;
  }

  auto& runtime = *runtime_it->second;
  SchedulerReplacementOutcome outcome = SchedulerReplacementOutcome::Rejected;
  try {
    outcome =
        runtime
            .submit_compute_request([this, &runtime, intent,
                                     type]() -> SchedulerReplacementOutcome {
              const std::optional<SchedulerPlan> plan =
                  SchedulerFactory::plan(type, scheduler_config_.worker_count);
              if (!plan.has_value()) {
                return SchedulerReplacementOutcome::Rejected;
              }

              GraphRuntime::SchedulerExecutionRoute execution_route;
              if (plan->is_builtin_cpu()) {
                try {
                  execution_service_->configure_worker_count(
                      scheduler_config_.worker_count);
                } catch (const GraphError& error) {
                  if (error.code() == GraphErrc::ComputeError) {
                    return SchedulerReplacementOutcome::ResourceExhausted;
                  }
                  throw;
                }
                execution_route.domain = GraphRuntime::SchedulerExecutionRoute::
                    Domain::ProcessCpuService;
                runtime.replace_scheduler(intent, nullptr, execution_route);
                return SchedulerReplacementOutcome::Success;
              }

              auto reservation =
                  execution_service_->try_reserve_legacy_scheduler_workers(
                      plan->reservation_slots());
              if (!reservation.has_value()) {
                return SchedulerReplacementOutcome::ResourceExhausted;
              }

              auto scheduler =
                  SchedulerFactory::create(*plan, std::move(*reservation));
              if (!scheduler) {
                return SchedulerReplacementOutcome::Rejected;
              }
              runtime.replace_scheduler(intent, std::move(scheduler),
                                        execution_route);
              return SchedulerReplacementOutcome::Success;
            })
            .get();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return false;
  }

  if (outcome == SchedulerReplacementOutcome::ResourceExhausted) {
    throw GraphError(GraphErrc::ComputeError,
                     "Host resource ledger cannot admit scheduler replacement");
  }
  return outcome == SchedulerReplacementOutcome::Success;
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
    return runtime
        .submit_compute_request(
            [this, &runtime,
             intent]() -> std::optional<std::pair<std::string, std::string>> {
              const GraphRuntime::SchedulerExecutionRoute route =
                  runtime.get_scheduler_execution_route(intent);
              if (route.domain == GraphRuntime::SchedulerExecutionRoute::
                                      Domain::ProcessCpuService) {
                return std::make_pair(std::string("CpuWorkStealingScheduler"),
                                      execution_service_->get_stats());
              }
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
