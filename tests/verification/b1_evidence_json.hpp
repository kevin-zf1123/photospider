/**
 * @file b1_evidence_json.hpp
 * @brief Declares verification-only JSON encoding for closed B1 evidence.
 */
#pragma once

#include <nlohmann/json.hpp>

#include "benchmark/b1_evidence.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

/**
 * @brief Encodes the complete immutable B1 workload contract.
 * @return Frozen workload, corpus, limits, and threshold object.
 * @throws nlohmann/std allocation failures unchanged.
 */
nlohmann::json b1_workload_contract_json();

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
