#pragma once

#include <cstddef>
#include <cstdint>

#include "01-numeric/sequence_profiles.hpp"

namespace ps::plugin_internal::numeric_ops {
// Compare four independent unsigned integer keys without numeric conversions.
// Inputs and outputs are exactly four slots in admitted continuation scratch.
void compare_keys(const std::uint64_t* a, const std::uint64_t* b,
                  std::int64_t* greater, std::int64_t* less,
                  SequenceProfile profile);
// Produces four replicas in private scratch; only the selected branch was read.
void select_words(std::uint64_t* result, std::uint64_t when_true,
                  std::uint64_t when_false, std::uint8_t condition,
                  SequenceProfile profile);
const char* numeric_build_identity();
const char* selection_implementation(SequenceProfile profile);
const char* comparison_implementation(SequenceProfile profile);
}  // namespace ps::plugin_internal::numeric_ops
