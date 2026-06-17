#pragma once

#include <functional>
#include <optional>
#include <string>

#include "ps_types.hpp"

namespace ps {
class GraphRuntime;
class SchedulerTaskRuntime;
}  // namespace ps

namespace ps::compute {

struct IntentUpdateDecision {
  /** @brief Intent being coordinated. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  /** @brief Whether the intent requires an explicit dirty ROI. */
  bool requires_dirty_roi = false;
  /** @brief Whether the HP sibling update should run. */
  bool run_high_precision_update = false;
  /** @brief Whether the RT sibling update should run. */
  bool run_real_time_update = false;
  /** @brief Whether HP and RT dirty siblings may start concurrently. */
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
 * @note Concurrent Dirty RT coordination starts HP/RT sibling callbacks with
 * std::async after confirming both scheduler task runtimes are alive. Each
 * callback then pushes its own ready TaskHandle batches to the matching
 * scheduler runtime.
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
};

/**
 * @brief Coordinates compute intent callbacks without owning task graphs.
 *
 * The coordinator decides which intent paths must run and whether HP/RT
 * RealTimeUpdate siblings may execute concurrently. Actual task graph planning,
 * dependency release, and scheduler submission remain inside the callbacks.
 *
 * @note Concurrent sibling execution uses std::async only to start the HP and
 * RT dirty callbacks together. Scheduler queues receive the fine-grained ready
 * work emitted by those callbacks through their intent-specific dispatchers.
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
   * HP/RT siblings can start concurrently.
   * @param rt_task_runtime Optional RT scheduler runtime used to decide whether
   * HP/RT siblings can start concurrently.
   * @param dirty_roi Optional dirty ROI for dirty intents.
   * @param callbacks Callback bundle that performs actual compute work.
   * @return Output for the requested target after required paths complete.
   * @throws GraphError for invalid inputs or missing callbacks; rethrows
   * callback exceptions after both sibling paths have been joined.
   * @note The coordinator never owns scheduler dependency state. It records
   * sibling submission, wait, and completion stages, while callbacks submit
   * concrete dirty task batches to scheduler queues.
   */
  static NodeOutput& coordinate_intent_update(
      ComputeIntent intent, SchedulerTaskRuntime* hp_task_runtime,
      SchedulerTaskRuntime* rt_task_runtime,
      const std::optional<cv::Rect>& dirty_roi,
      const IntentUpdateCallbacks& callbacks);
};

}  // namespace ps::compute
