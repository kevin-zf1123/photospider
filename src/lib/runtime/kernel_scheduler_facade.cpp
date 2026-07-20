/**
 * @file kernel_scheduler_facade.cpp
 * @brief Implements Kernel scheduler replacement and scheduler inspection
 * facades.
 */
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "photospider/core/graph_error.hpp"
#include "runtime/kernel.hpp"
#include "scheduler/scheduler_factory.hpp"
#include "scheduler/scheduler_worker_budget.hpp"

namespace ps {
namespace {

/**
 * @brief Internal result of one serialized scheduler replacement attempt.
 *
 * @throws Nothing for value construction and comparison.
 * @note The outcome distinguishes only the newly specified budget exhaustion
 * category from legacy handled rejection. Candidate/plugin GraphError and
 * ordinary lifecycle failures remain folded into `Rejected` by the outer
 * Kernel boundary, preserving behavior outside issue #43's frozen matrix.
 */
enum class SchedulerReplacementOutcome {
  /** @brief Candidate published and the displaced owner cleanup completed. */
  Success,
  /** @brief Type, factory, or handled candidate lifecycle rejected the call. */
  Rejected,
  /** @brief A valid candidate plan could not reserve transient capacity. */
  BudgetExhausted,
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
        runtime.graph_state()
            .submit([this, &runtime, intent,
                     type](GraphModel&) -> SchedulerReplacementOutcome {
              const std::optional<SchedulerPlan> plan =
                  SchedulerFactory::plan(type, scheduler_config_.worker_count);
              if (!plan.has_value()) {
                return SchedulerReplacementOutcome::Rejected;
              }

              auto reservation = SchedulerWorkerBudget::process().try_reserve(
                  plan->reservation_slots());
              if (!reservation.has_value()) {
                return SchedulerReplacementOutcome::BudgetExhausted;
              }

              auto scheduler =
                  SchedulerFactory::create(*plan, std::move(*reservation));
              if (!scheduler) {
                return SchedulerReplacementOutcome::Rejected;
              }
              GraphRuntime::SchedulerExecutionRoute execution_route;
              if (intent == ComputeIntent::GlobalHighPrecision &&
                  plan->is_builtin_cpu()) {
                execution_route.domain = GraphRuntime::SchedulerExecutionRoute::
                    Domain::ProcessCpuService;
                execution_route.worker_count = plan->worker_grant();
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

  if (outcome == SchedulerReplacementOutcome::BudgetExhausted) {
    throw GraphError(
        GraphErrc::ComputeError,
        "process scheduler worker budget cannot admit replacement");
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
