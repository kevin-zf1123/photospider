/**
 * @file b1_evidence_json.hpp
 * @brief Declares verification-only JSON encoding for closed B1 evidence.
 */
#pragma once

#include <nlohmann/json.hpp>

#include "benchmark/b1/b1_evidence.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

/**
 * @brief Encodes the complete immutable B1 workload contract.
 * @return Frozen workload, corpus, limits, and threshold object.
 * @throws nlohmann/std allocation failures unchanged.
 */
nlohmann::json b1_workload_contract_json();

/**
 * @brief Encodes one complete raw cold/warmup/measured B1 occurrence.
 * @param evidence Exact job, physical, execution, output, golden, and semantic
 * evidence.
 * @return Closed source-faithful job object without derived row aggregates.
 * @throws nlohmann/std allocation failures unchanged.
 * @note This helper lets a composing verification profile retain the Issue
 * #95 source record without defining a second B1 evidence grammar.
 */
nlohmann::json b1_job_evidence_json(const B1JobEvidence& evidence);

/**
 * @brief Encodes one fully evaluated closed Issue #95 inner row.
 * @param row Raw and derived B1 evidence.
 * @return Closed version-one JSON object; never an outer canonical row.
 * @throws nlohmann/std allocation failures unchanged.
 * @note Every raw manifest, trace, physical event, I/O event, receipt, and
 * execution snapshot retained by the C++ model is serialized.
 */
nlohmann::json b1_inner_row_json(const B1InnerRow& row);

}  // namespace ps::benchmark
