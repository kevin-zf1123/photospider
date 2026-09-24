#pragma once

#include <cstddef>

namespace ps::plugin_internal::numeric_ops {
// Private ISA-admitted normal-range kernel. Inputs must be finite [-80,80],
// caller must establish nearest/gradual mode. No source overread on tails.
void exp_simd_f32(const float* input, float* output, std::size_t count);
}  // namespace ps::plugin_internal::numeric_ops
