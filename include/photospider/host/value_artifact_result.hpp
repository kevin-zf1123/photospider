#pragma once

#include "photospider/data/value_artifact.hpp"
#include "photospider/host/value_result.hpp"

/**
 * @file value_artifact_result.hpp
 * @brief Transactional bridge between Host named results and portable Values.
 */

namespace ps {

/**
 * @brief Captures every Ready Value in one exact Host result transactionally.
 * @param result Valid canonically ordered terminal result.
 * @return Complete owned artifact set in exact name order.
 * @throws All per-Value capture and allocation failures unchanged.
 * @note No artifact set escapes until every Value has been copied and checked.
 */
NamedValueArtifactSet capture_named_value_artifact_set(
    const NamedValueResult& result);

/**
 * @brief Transactionally reconstructs one complete canonical Host result.
 * @param artifacts Detached validated named artifact set.
 * @param registry Provider registry required by provider-defined Values.
 * @return Fresh Ready named Values only after every artifact validates.
 * @throws All artifact, provider, payload, and result validation failures.
 * @note Partial Values remain constructor-local and never escape on failure.
 */
NamedValueResult reconstruct_named_value_artifact_set(
    const NamedValueArtifactSet& artifacts,
    DataDefinitionRegistry* registry = nullptr);

}  // namespace ps
