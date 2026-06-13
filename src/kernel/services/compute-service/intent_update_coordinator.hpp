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
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  bool requires_dirty_roi = false;
  bool run_high_precision_update = false;
  bool run_real_time_update = false;
  bool submit_updates_concurrently = false;
};

struct IntentUpdateCallbacks {
  std::function<NodeOutput&()> run_global_high_precision;
  std::function<NodeOutput&()> run_global_high_precision_dirty_update;
  std::function<void()> run_high_precision_update;
  std::function<NodeOutput&()> run_real_time_update;
  std::function<NodeOutput&()> real_time_output;
  std::function<void(const std::string&)> record_stage;
};

class IntentUpdateCoordinator {
 public:
  static IntentUpdateDecision decide(ComputeIntent intent,
                                     bool can_submit_concurrently,
                                     bool has_dirty_roi);
  static void validate(ComputeIntent intent,
                       const std::optional<cv::Rect>& dirty_roi);
  static NodeOutput& coordinate_intent_update(
      ComputeIntent intent, SchedulerTaskRuntime* hp_task_runtime,
      SchedulerTaskRuntime* rt_task_runtime,
      const std::optional<cv::Rect>& dirty_roi,
      const IntentUpdateCallbacks& callbacks);
};

}  // namespace ps::compute
