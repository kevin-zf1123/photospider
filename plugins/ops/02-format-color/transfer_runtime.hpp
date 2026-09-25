#pragma once

#include "01-numeric/sequence_profiles.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::transfer_ops {
// Private registration factory, shared with focused runtime contract tests.
OperationDefinition operation(bool encode, numeric_ops::SequenceProfile profile,
                              const char* suffix);
}  // namespace ps::plugin_internal::transfer_ops
