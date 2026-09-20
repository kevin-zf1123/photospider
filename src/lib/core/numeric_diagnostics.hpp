#pragma once

#include "photospider/core/numeric_diagnostics.hpp"

namespace ps::numeric_internal {
// Both snapshots come from the same checked monotone session accumulator.
// Emitting each poll's delta preserves work if subsequent input supply fails.
inline NumericDiagnostics delta(const NumericDiagnostics& current,
                                NumericDiagnostics* previous) {
  auto result = current;
  result.strict_math_calls -= previous->strict_math_calls;
  for (unsigned function = 0; function < 8; ++function)
    for (unsigned reason = 0; reason < 4; ++reason)
      result.function_fallbacks[function][reason] -=
          previous->function_fallbacks[function][reason];
  result.evaluated_values -= previous->evaluated_values;
  result.strict_fallbacks -= previous->strict_fallbacks;
  result.view_elements -= previous->view_elements;
  result.copied_elements -= previous->copied_elements;
  for (unsigned i = 0; i < result.fallback_reasons.size(); ++i)
    result.fallback_reasons[i] -= previous->fallback_reasons[i];
  *previous = current;
  return result;
}
}  // namespace ps::numeric_internal
