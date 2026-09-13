#pragma once

#include <cstdint>

#include "01-numeric/exact_sequence.hpp"
#include "photospider/core/status.hpp"

namespace ps::plugin_internal::numeric_ops {
enum class SequenceProfile { Strict, AppleSilicon, X86Avx2 };
Status sequence_profile_available(SequenceProfile profile);
const char* sequence_implementation();
/** Multiplies 68 limbs by a small exact factor, using supplied product scratch.
 * Requires a successful capability check before entering the selected ISA.
 * Scratch holds 68 uint64 products and belongs to the host continuation.
 */
void sequence_multiply(ExactSequence* value, std::uint32_t factor,
                       SequenceProfile profile, std::uint64_t* products);
}  // namespace ps::plugin_internal::numeric_ops
