/**
 * @file i1_evidence_json.hpp
 * @brief Declares verification-only JSON encoding for closed I1 evidence.
 */
#pragma once

#include <nlohmann/json.hpp>

#include "benchmark/i1/i1_evidence.hpp"

namespace ps::benchmark {

/**
 * @brief Encodes the complete frozen I1 workload and grid contract.
 * @return Closed JSON contract object independent of runner defaults.
 * @throws nlohmann/std allocation errors unchanged.
 * @note This verification helper is shared by the manual runner and focused
 * serializer tests; it is not part of the installable product contract.
 */
nlohmann::json i1_workload_contract_json();

/**
 * @brief Returns the stable token for one I1 grid-slot phase.
 * @param phase Typed cold, warmup, or measured phase.
 * @return Stable lowercase phase token.
 * @throws Nothing.
 */
const char* i1_phase_text(I1EpisodePhase phase) noexcept;

/**
 * @brief Encodes one fully evaluated closed Issue #93 inner row.
 * @param row Raw and derived episode evidence.
 * @return Closed version-one JSON object; never an outer canonical row.
 * @throws nlohmann/std allocation failures unchanged.
 * @note Absent admission, accepted-product, and observation facts remain
 * explicit JSON nulls or empty arrays rather than synthesized success facts.
 * Visible-output digests must already be frozen; serialization never traverses
 * retained Value payloads.
 */
nlohmann::json i1_inner_row_json(const I1EpisodeInnerRow& row);

/**
 * @brief Encodes one exact replicate aggregate and frozen gate thresholds.
 * @param summary Evaluated 221-slot aggregate.
 * @return Closed summary JSON object.
 * @throws nlohmann/std allocation errors unchanged.
 */
nlohmann::json i1_replicate_summary_json(const I1ReplicateSummary& summary);

}  // namespace ps::benchmark
