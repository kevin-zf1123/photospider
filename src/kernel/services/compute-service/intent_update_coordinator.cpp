#include "kernel/services/compute-service/intent_update_coordinator.hpp"

#include <exception>
#include <future>
#include <memory>
#include <utility>

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
        can_submit_concurrently && has_dirty_roi;
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

      if (!decision.submit_updates_concurrently) {
        record_stage(callbacks, "intent_coordinator_inline_hp");
        if (decision.run_high_precision_update) {
          callbacks.run_high_precision_update();
        }
        record_stage(callbacks, "intent_coordinator_inline_rt");
        if (decision.run_real_time_update) {
          return callbacks.run_real_time_update();
        }
        return callbacks.real_time_output();
      }

      record_stage(callbacks, "intent_coordinator_submit_hp_scheduler_runtime");
      auto hp_future = std::async(
          std::launch::async, [run_hp = callbacks.run_high_precision_update,
                               record = callbacks.record_stage]() {
            try {
              if (record) {
                record("intent_coordinator_run_hp_scheduler_runtime");
              }
              run_hp();
              if (record) {
                record("intent_coordinator_hp_scheduler_runtime_done");
              }
            } catch (...) {
              throw;
            }
          });

      record_stage(callbacks, "intent_coordinator_submit_rt_scheduler_runtime");
      auto rt_future = std::async(
          std::launch::async, [run_rt = callbacks.run_real_time_update,
                               record = callbacks.record_stage]() {
            try {
              if (record) {
                record("intent_coordinator_run_rt_scheduler_runtime");
              }
              run_rt();
              if (record) {
                record("intent_coordinator_rt_scheduler_runtime_done");
              }
            } catch (...) {
              throw;
            }
          });

      std::exception_ptr first_error;
      try {
        record_stage(callbacks, "intent_coordinator_wait_rt_scheduler_runtime");
        rt_future.get();
      } catch (...) {
        first_error = std::current_exception();
      }
      try {
        record_stage(callbacks, "intent_coordinator_wait_hp_scheduler_runtime");
        hp_future.get();
      } catch (...) {
        if (!first_error) {
          first_error = std::current_exception();
        }
      }
      if (first_error) {
        std::rethrow_exception(first_error);
      }

      record_stage(callbacks, "intent_coordinator_scheduler_runtime_complete");
      return callbacks.real_time_output();
    }
  }

  require_callback(callbacks.run_global_high_precision,
                   "run_global_high_precision");
  return callbacks.run_global_high_precision();
}

}  // namespace ps::compute
