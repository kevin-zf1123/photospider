/**
 * @file m1_canonical.hpp
 * @brief Declares reversible canonical M1 inner evidence materialization.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "benchmark/m1/m1_evidence.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

/**
 * @brief Independently replayed canonical M1 inner evidence.
 * @throws std::bad_alloc when retained evidence or diagnostics are copied.
 */
struct M1CanonicalReplay final {
  /** @brief Reconstructed raw evidence and freshly recomputed six verdicts. */
  M1InnerRow row;
  /** @brief Closed producer observation records cross-checked with starts. */
  M1FairnessObservationSnapshot observations;
};

/**
 * @brief Materializes one evaluated M1 row into the closed reversible schema.
 * @param row Evaluated row retaining the complete evaluator input.
 * @param observations Complete producer observation snapshot backing class
 * starts and observer-state flags.
 * @return Exact canonical `execution-profile-m1-inner-row-v2` bytes.
 * @throws std::invalid_argument for unknown enums, noncanonical values,
 * inconsistent observation/start evidence, or a stale supplied verdict.
 * @throws std::overflow_error when a signed monotonic time cannot be retained.
 * @throws std::bad_alloc when canonical records allocate.
 * @note The bytes retain authority-free receipt observations but no output-
 * receipt capability, storage, scheduling, or machine authority. They retain
 * every value consumed by the five-axis evaluator and are emitted only after
 * source-derived progress, Graph, headroom, and aggregate projections close.
 */
std::string materialize_m1_inner_row(
    const M1InnerRow& row, const M1FairnessObservationSnapshot& observations);

/**
 * @brief Strictly parses, reconstructs, and independently reevaluates M1 bytes.
 * @param canonical_bytes Exact closed M1 inner canonical manifest.
 * @param expected_replicate_ordinal Enclosing outer-row ordinal in `[1,3]`.
 * @return Reconstructed authority-free input, observations, and fresh result.
 * @throws std::invalid_argument for unknown, duplicate, missing, reordered,
 * truncated, noncanonical, contradictory, non-one-second, or verdict-mismatch
 * evidence.
 * @throws std::bad_alloc when reconstructed evidence or diagnostics allocate.
 * @note Parsing reuses `evaluate_m1_inner_row`, the shared source-derived
 * fairness projection, and the shared Issue #95 I/O FSM. It does not create
 * portable output or live machine conformance claims.
 */
M1CanonicalReplay parse_and_recompute_m1_inner_row(
    std::string_view canonical_bytes, std::uint64_t expected_replicate_ordinal);

}  // namespace ps::benchmark
