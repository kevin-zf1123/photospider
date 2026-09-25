#include <vector>
#pragma once
#include "02-format-color/alpha_common.hpp"

namespace ps::plugin_internal {
// Host-only static preflight for transactional FMT-05 authoring expansion.
Result<OperationPreparation> prepare_alpha_extraction(
    const std::vector<OperationMetadata>&, const alpha_ops::Params&,
    numeric_ops::SequenceProfile);
Result<OperationPreparation> prepare_alpha_channel_mapping(
    const std::vector<OperationMetadata>&, const alpha_ops::Params&,
    numeric_ops::SequenceProfile);
Result<OperationPreparation> prepare_channel_literal_like(
    const std::vector<OperationMetadata>&, const alpha_ops::Params&,
    numeric_ops::SequenceProfile);
Status register_channel_literal_like(OperationRegistry*);
}  // namespace ps::plugin_internal
