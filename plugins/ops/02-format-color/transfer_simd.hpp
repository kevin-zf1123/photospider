#pragma once

#include <cstdint>

namespace ps::plugin_internal::transfer_ops {
// Requires the existing AVX2+FMA / AArch64 capability gate, an active nearest
// environment with gradual underflow, and count authorized contiguous values.
// Signed gamma=2 square/sqrt. Integer masks quiet NaNs preserving payload/sign;
// no signaling NaN is used as a floating operand. Never reads a padded lane.
void gamma2_simd(const std::uint8_t* input, std::uint8_t* output,
                 unsigned count, bool narrow, bool encode);
}  // namespace ps::plugin_internal::transfer_ops
