#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphRuntime;
class SchedulerTaskRuntime;
}  // namespace ps

namespace ps::compute {
class DirtySiblingCommitGate;

struct IntentUpdateDecision {
  /** @brief Intent being coordinated. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  /** @brief Whether the intent requires an explicit dirty ROI. */
  bool requires_dirty_roi = false;
  /** @brief Whether the HP sibling update should run. */
  bool run_high_precision_update = false;
  /** @brief Whether the RT sibling update should run. */
  bool run_real_time_update = false;
  /**
   * @brief Whether HP and RT dirty siblings may start concurrently.
   *
   * @note This is true only when both sibling scheduler runtimes are available
   * and dirty output commits are protected by staged buffers plus the sibling
   * commit gate.
   */
  bool submit_updates_concurrently = false;
};

/**
 * @brief Callback bundle used by IntentUpdateCoordinator.
 *
 * The coordinator owns no graph state. It validates intent policy, chooses
 * inline versus sibling execution, records coarse coordination stages, and
 * invokes these callbacks to let ComputeService build and dispatch the real
 * task-level HP/RT work.
 *
 * @note Concurrent RealTimeUpdate uses sibling_commit_gate to let HP compute
 * overlap RT while delaying HP GraphModel commit until RT proxy commit has
 * completed.
 */
struct IntentUpdateCallbacks {
  /** @brief Runs a normal full-graph HP compute. */
  std::function<NodeOutput&()> run_global_high_precision;
  /** @brief Runs an HP dirty update for a GlobalHighPrecision dirty ROI. */
  std::function<NodeOutput&()> run_global_high_precision_dirty_update;
  /** @brief Runs the HP sibling dirty update for RealTimeUpdate. */
  std::function<void()> run_high_precision_update;
  /** @brief Runs the RT sibling dirty update for RealTimeUpdate. */
  std::function<NodeOutput&()> run_real_time_update;
  /** @brief Returns the final RT output after sibling updates complete. */
  std::function<NodeOutput&()> real_time_output;
  /** @brief Records a coordinator stage in ComputeService event history. */
  std::function<void(const std::string&)> record_stage;
  /** @brief Optional gate shared by concurrent HP/RT dirty siblings. */
  std::shared_ptr<DirtySiblingCommitGate> sibling_commit_gate;
};

/**
 * @brief Coordinates compute intent callbacks without owning task graphs.
 *
 * The coordinator decides which intent paths must run. Under the staged commit
 * policy it starts RT before HP and overlaps HP with RT when both scheduler
 * runtimes are available. Actual task graph planning, dependency release,
 * graph mutation, and scheduler submission remain inside the callbacks.
 *
 * @note Scheduler queues still receive the fine-grained ready work emitted by
 * each callback through their intent-specific dispatchers.
 */
class IntentUpdateCoordinator {
 public:
  /**
   * @brief Builds the execution decision for one requested intent.
   *
   * @param intent Requested compute intent.
   * @param can_submit_concurrently Whether HP and RT runtimes are available and
   * running.
   * @param has_dirty_roi Whether the request supplied a dirty ROI.
   * @return Decision describing required HP/RT paths and execution mode.
   * @throws Nothing directly.
   */
  static IntentUpdateDecision decide(ComputeIntent intent,
                                     bool can_submit_concurrently,
                                     bool has_dirty_roi);

  /**
   * @brief Validates intent-specific request parameters.
   *
   * @param intent Requested compute intent.
   * @param dirty_roi Optional dirty ROI supplied by the caller.
   * @throws GraphError when RealTimeUpdate lacks a non-empty dirty ROI.
   */
  static void validate(ComputeIntent intent,
                       const std::optional<cv::Rect>& dirty_roi);

  /**
   * @brief Executes the callbacks required by one compute intent.
   *
   * @param intent Requested compute intent.
   * @param hp_task_runtime Optional HP scheduler runtime used to decide whether
   * scheduler-backed work may run inside each sibling callback.
   * @param rt_task_runtime Optional RT scheduler runtime used to decide whether
   * scheduler-backed work may run inside each sibling callback.
   * @param dirty_roi Optional dirty ROI for dirty intents.
   * @param callbacks Callback bundle that performs actual compute work.
   * @return Output for the requested target after required paths complete.
   * @throws std::bad_alloc when callback execution or async state exhausts
   * memory, including a sibling HP failure observed during RT-failure cleanup.
   * @throws GraphError for invalid inputs or missing callbacks; rethrows other
   * callback exceptions from the active sibling path.
   * @note The coordinator never owns scheduler dependency state. It records
   * sibling stages, while callbacks submit concrete dirty task batches to
   * scheduler queues. Resource exhaustion takes precedence over an already
   * active recoverable sibling failure so it cannot be silently discarded.
   */
  static NodeOutput& coordinate_intent_update(
      ComputeIntent intent, SchedulerTaskRuntime* hp_task_runtime,
      SchedulerTaskRuntime* rt_task_runtime,
      const std::optional<cv::Rect>& dirty_roi,
      const IntentUpdateCallbacks& callbacks);
};

}  // namespace ps::compute
