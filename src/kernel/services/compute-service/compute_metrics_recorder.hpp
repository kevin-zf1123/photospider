#pragma once

#include <vector>

#include "ps_types.hpp"

namespace ps::compute {

class ComputeMetricsRecorder {
 public:
  static void finalize_output_metadata(
      NodeOutput& output, const std::vector<const NodeOutput*>& inputs,
      bool enable_timing, double execution_ms);
};

}  // namespace ps::compute
