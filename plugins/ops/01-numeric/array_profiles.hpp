#pragma once

#include <cstddef>
#include <cstdint>

#include "01-numeric/sequence_profiles.hpp"

namespace ps::plugin_internal::numeric_ops {
// Copies an admitted block of at most 32 bytes. Full blocks use the selected
// ISA; exact byte tails never read or write outside the authorized span.
void array_copy_block(std::uint8_t* destination, const std::uint8_t* source,
                      std::size_t bytes, SequenceProfile profile);
const char* array_implementation(SequenceProfile profile, bool view);
}  // namespace ps::plugin_internal::numeric_ops
