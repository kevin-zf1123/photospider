#include "compute/intent_update_coordinator.hpp"

#include <future>
#include <new>
#include <string>

#include "compute/dirty_sibling_commit_gate.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

namespace ps::compute {
namespace {

void record_stage(const IntentUpdateCallbacks& callbacks,
                  const std::string& stage) {
  if (callbacks.record_stage) {
    callbacks.record_stage(stage);
  }
}

template <typename Fn>
void require_callback(const Fn& fn, const std::string& name) {
  if (!fn) {
    throw GraphError(GraphErrc::ComputeError,
                     "IntentUpdateCoordinator missing callback: " + name);
  }
}

}  // namespace

IntentUpdateDecision IntentUpdateCoordinator::decide(
    ComputeIntent intent, bool can_submit_concurrently, bool has_dirty_roi) {
  IntentUpdateDecision decision;
  decision.intent = intent;
  if (intent == ComputeIntent::RealTimeUpdate) {
    decision.requires_dirty_roi = true;
    decision.run_high_precision_update = has_dirty_roi;
    decision.run_real_time_update = has_dirty_roi;
    decision.submit_updates_concurrently =
        has_dirty_roi && can_submit_concurrently;
  }
  return decision;
}

void IntentUpdateCoordinator::validate(
    ComputeIntent intent, const std::optional<cv::Rect>& dirty_roi) {
  if (intent != ComputeIntent::RealTimeUpdate)
    return;
  if (!dirty_roi.has_value()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "RealTimeUpdate intent requires a dirty ROI region.");
  }
  if (dirty_roi->width <= 0 || dirty_roi->height <= 0) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "RealTimeUpdate intent requires a non-empty dirty ROI.");
  }
}

/**
 * @brief Executes the callback set required by one compute intent.
 *
 * @param intent Requested global-HP or realtime intent.
 * @param hp_task_runtime Optional HP runtime used for concurrency capability.
 * @param rt_task_runtime Optional RT runtime used for concurrency capability.
 * @param dirty_roi Optional dirty ROI validated for realtime requests.
 * @param callbacks Borrowed compute, output, trace, and commit-gate callbacks.
 * @return Target output after the selected intent paths complete.
 * @throws std::bad_alloc when callback execution or async state exhausts
 * memory, including an HP sibling failure observed while handling RT failure.
 * @throws GraphError for invalid requests or missing callbacks.
 * @throws std::exception for other callback or future failures.
 * @note Concurrent failure aborts staged HP commit, drains the HP future, and
 * preserves resource-exhaustion identity before rethrowing the primary error.
 */
NodeOutput& IntentUpdateCoordinator::coordinate_intent_update(
    ComputeIntent intent, SchedulerTaskRuntime* hp_task_runtime,
    SchedulerTaskRuntime* rt_task_runtime,
    const std::optional<cv::Rect>& dirty_roi,
    const IntentUpdateCallbacks& callbacks) {
  switch (intent) {
    case ComputeIntent::GlobalHighPrecision:
      if (dirty_roi.has_value()) {
        require_callback(callbacks.run_global_high_precision_dirty_update,
                         "run_global_high_precision_dirty_update");
        record_stage(callbacks, "intent_coordinator_global_dirty_update");
        return callbacks.run_global_high_precision_dirty_update();
      }
      require_callback(callbacks.run_global_high_precision,
                       "run_global_high_precision");
      record_stage(callbacks, "intent_coordinator_global_high_precision");
      return callbacks.run_global_high_precision();

    case ComputeIntent::RealTimeUpdate: {
      validate(intent, dirty_roi);
      require_callback(callbacks.run_high_precision_update,
                       "run_high_precision_update");
      require_callback(callbacks.run_real_time_update, "run_real_time_update");
      require_callback(callbacks.real_time_output, "real_time_output");
      const bool can_submit_concurrently =
          hp_task_runtime != nullptr && rt_task_runtime != nullptr &&
          hp_task_runtime->task_runtime_running() &&
          rt_task_runtime->task_runtime_running();
      const IntentUpdateDecision decision =
          decide(intent, can_submit_concurrently, dirty_roi.has_value());
      record_stage(callbacks, decision.submit_updates_concurrently
                                  ? "intent_coordinator_decision_concurrent"
                                  : "intent_coordinator_decision_inline");

      if (decision.submit_updates_concurrently) {
        record_stage(callbacks, "intent_coordinator_concurrent_rt_start");
        auto rt_future = std::async(std::launch::async, [&]() -> NodeOutput* {
          if (decision.run_real_time_update) {
            return &callbacks.run_real_time_update();
          }
          return &callbacks.real_time_output();
        });

        record_stage(callbacks, "intent_coordinator_concurrent_hp_start");
        auto hp_future = std::async(std::launch::async, [&]() {
          if (decision.run_high_precision_update) {
            callbacks.run_high_precision_update();
          }
        });

        try {
          NodeOutput* rt_output = rt_future.get();
          record_stage(callbacks, "intent_coordinator_concurrent_rt_done");
          hp_future.get();
          record_stage(callbacks, "intent_coordinator_concurrent_hp_done");
          return *rt_output;
        } catch (...) {
          if (callbacks.sibling_commit_gate) {
            callbacks.sibling_commit_gate->abort_hp_commit();
          }
          try {
            hp_future.get();
          } catch (const std::bad_alloc&) {
            throw;
          } catch (...) {
          }
          throw;
        }
      }

      record_stage(callbacks, "intent_coordinator_inline_rt");
      if (decision.run_real_time_update) {
        NodeOutput& rt_output = callbacks.run_real_time_update();
        record_stage(callbacks, "intent_coordinator_inline_hp");
        if (decision.run_high_precision_update) {
          callbacks.run_high_precision_update();
        }
        return rt_output;
      }
      record_stage(callbacks, "intent_coordinator_inline_hp");
      if (decision.run_high_precision_update) {
        callbacks.run_high_precision_update();
      }
      return callbacks.real_time_output();
    }
  }

  require_callback(callbacks.run_global_high_precision,
                   "run_global_high_precision");
  return callbacks.run_global_high_precision();
}

}  // namespace ps::compute
