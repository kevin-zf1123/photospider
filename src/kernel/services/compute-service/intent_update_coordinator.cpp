#include "kernel/services/compute-service/intent_update_coordinator.hpp"

namespace ps::compute {

IntentUpdateDecision IntentUpdateCoordinator::decide(ComputeIntent intent,
                                                     bool runtime_running,
                                                     bool has_dirty_roi) {
  IntentUpdateDecision decision;
  decision.intent = intent;
  if (intent == ComputeIntent::RealTimeUpdate) {
    decision.requires_dirty_roi = true;
    decision.run_high_precision_update = has_dirty_roi;
    decision.run_real_time_update = has_dirty_roi;
    decision.submit_updates_concurrently = runtime_running && has_dirty_roi;
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

}  // namespace ps::compute
