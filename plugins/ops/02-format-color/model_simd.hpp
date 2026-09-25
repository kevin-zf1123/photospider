#pragma once
#include <array>
#include <cstddef>

#include "01-numeric/sequence_profiles.hpp"
namespace ps::plugin_internal::model_ops {
// Only supplied support lanes are loaded. Candidate arithmetic is never the
// correctness oracle: ModelMath certifies the complete expression separately.
void dot_candidates(const std::array<const double*, 3>& input,
                    const std::array<double, 3>& coefficients, unsigned mask,
                    double* output, std::size_t count,
                    numeric_ops::SequenceProfile profile, bool vectorize);
}  // namespace ps::plugin_internal::model_ops
