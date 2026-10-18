#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "photospider/compiler/ir.hpp"
#include "photospider/compiler/plan.hpp"

namespace ps {

/**
 * @brief Raw wall-clock compiler stage timings.
 *
 * @note Timings are diagnostics, not performance verdicts or release evidence.
 */
struct PHOTOSPIDER_API CompilationDiagnostics final {
  /** @brief Semantic analysis duration in microseconds. */
  std::uint64_t analyze_us = 0;
  /** @brief Optimization duration in microseconds. */
  std::uint64_t optimize_us = 0;
  /** @brief Physical planning duration in microseconds. */
  std::uint64_t plan_us = 0;
};

/** @brief Published stage identity and independently retained IR storage.
 * Reset ir only after successful export. Active consumers must retain their
 * own immutable copy before release; content identity and provenance survive.
 */
struct PHOTOSPIDER_API IRRevision final {
  IRRevision(std::size_t revision, ValidatedIR version)
      : index(revision),
        ir(std::move(version)),
        content_digest_(ir->digest()),
        provenance_(ir->provenance()) {}
  std::size_t index;
  std::optional<ValidatedIR> ir;
  const IRDigest& content_digest() const noexcept { return content_digest_; }
  const IRProvenance& provenance() const noexcept { return provenance_; }

 private:
  IRDigest content_digest_;
  IRProvenance provenance_;
};

/** @brief Retained immutable versions and an optional physical plan. */
struct PHOTOSPIDER_API CompiledWorkflow final {
  std::vector<IRRevision> ir_versions;
  std::optional<ExecutionPlan> plan;
  CompilationDiagnostics diagnostics;
};

}  // namespace ps
