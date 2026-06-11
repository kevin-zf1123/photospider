#pragma once

#include <optional>

#include "ps_types.hpp"

namespace ps::compute {

struct IntentUpdateDecision {
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  bool requires_dirty_roi = false;
  bool run_high_precision_update = false;
  bool run_real_time_update = false;
  bool submit_updates_concurrently = false;
};

class IntentUpdateCoordinator {
 public:
  static IntentUpdateDecision decide(ComputeIntent intent, bool runtime_running,
                                     bool has_dirty_roi);
  static void validate(ComputeIntent intent,
                       const std::optional<cv::Rect>& dirty_roi);
};

}  // namespace ps::compute
