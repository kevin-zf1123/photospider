/**
 * @file i2_evidence_json.hpp
 * @brief Declares verification-only JSON encoding for closed I2 evidence.
 */
#pragma once

#include <nlohmann/json.hpp>

#include "benchmark/i2_evidence.hpp"

namespace ps::benchmark {

/**
 * @brief Returns the stable token for one I2 grid-slot phase.
 * @param phase Typed cold, warmup, or measured phase.
 * @return Stable lowercase phase token.
 * @throws Nothing.
 */
const char* i2_phase_text(I2EpisodePhase phase) noexcept;

/**
 * @brief Encodes the complete frozen I2 workload and grid contract.
 * @return Closed JSON contract object independent of runner defaults.
 * @throws nlohmann/std allocation failures unchanged.
 */
nlohmann::json i2_workload_contract_json();

/**
 * @brief Encodes one fully evaluated closed Issue #94 inner row.
 * @param row Raw and derived episode evidence.
 * @return Closed version-one JSON object; never an outer canonical row.
 * @throws nlohmann/std allocation failures unchanged.
 * @note Serialization traverses no Value payload and preserves every absent
 * fact explicitly as null or an empty array.
 */
nlohmann::json i2_inner_row_json(const I2EpisodeInnerRow& row);

/**
 * @brief Encodes one exact I2 replicate aggregate and frozen thresholds.
 * @param summary Evaluated 111-slot aggregate.
 * @return Closed summary JSON object.
 * @throws nlohmann/std allocation failures unchanged.
 */
nlohmann::json i2_replicate_summary_json(const I2ReplicateSummary& summary);

}  // namespace ps::benchmark
