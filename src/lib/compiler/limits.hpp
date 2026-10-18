#pragma once

#include "photospider/data/footprint.hpp"

namespace ps::compiler_internal {
// Compilation and text loading share the same bounded geometry envelope.
// Execution has its own per-Run set limits and never expands these mappings.
inline FootprintLimits static_set_limits() {
  FootprintLimits limits;
  limits.maximum_boxes = 4194304;
  limits.maximum_work = 268435456;
  return limits;
}
}  // namespace ps::compiler_internal
