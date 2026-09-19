#pragma once

#include <array>
#include <cstdint>

#include "photospider/core/status.hpp"

namespace ps {
/** @brief Actual CPU arithmetic path, separate from the CPU/GPU scheduler. */
enum class CpuNumericProfile : std::uint32_t {
  Unspecified = 0,
  Strict = 1,
  AppleSiliconNeon = 2,
  X86Avx2 = 3
};
/** @brief Bounded numerical fallback categories; resource/upstream failures
 * are never recoverable numerical fallbacks.
 */
enum class NumericFallbackReason : std::uint32_t {
  RoundingUnresolved = 0,
  FunctionUnsupported = 1,
  SpecialValueProtection = 2,
  PrecisionRefinement = 3
};
/** @brief Owning bounded diagnostics for actual arithmetic performed.
 * implementation is a NUL-terminated identity containing library/revision,
 * compiler, build options and OS/architecture. No borrowed string or callback
 * survives a session. Counts exclude cache hits and undemanded observations.
 * Per-reason counts sum to strict_fallbacks. None of these fields changes
 * numerical semantic identity or proves an accuracy bound.
 */
struct NumericDiagnostics final {
  CpuNumericProfile profile = CpuNumericProfile::Unspecified;
  std::array<char, 256> implementation{};
  /** @brief Admitted numeric evaluations, including later failed attempts.
   * Most operations count output-value attempts. Exact reductions and
   * cumulative scans count accumulator input attempts; metadata-only
   * reduce_count reports zero. OperationTiming::computed_elements separately
   * counts output elements.
   */
  std::uint64_t evaluated_values = 0;
  std::uint64_t strict_fallbacks = 0;
  /** @brief Actual logical elements formed for publication as views or packed
   * copies. Unused for arithmetic-only operations; excludes collection outside
   * the operator and cache hits. Counts survive a later attempt failure. A view
   * count does not imply allocated payload.
   */
  std::uint64_t view_elements = 0;
  std::uint64_t copied_elements = 0;
  std::array<std::uint64_t, 4> fallback_reasons{};
};
/** @brief Checks and accumulates a physical attempt in fixed inline storage.
 * Identities must match, except that a canonical empty destination adopts its
 * first report. Overflow/malformed input leaves the destination unchanged.
 * Caller serializes mutations; independent records can be merged concurrently.
 * Only construction of a failure diagnostic can allocate.
 */
inline Status merge_numeric_diagnostics(NumericDiagnostics* target,
                                        const NumericDiagnostics& report) {
  if (!target)
    return Status{ErrorCode::InvalidArgument, "null numeric diagnostics"};
  if (report.profile == CpuNumericProfile::Unspecified) {
    if (report.implementation != std::array<char, 256>{} ||
        report.evaluated_values || report.strict_fallbacks ||
        report.view_elements || report.copied_elements ||
        report.fallback_reasons != std::array<std::uint64_t, 4>{})
      return Status{ErrorCode::InvalidArgument,
                    "noncanonical empty numeric diagnostics"};
    return Status::success();
  }
  if (static_cast<std::uint32_t>(report.profile) > 3 ||
      !report.implementation[0] || report.implementation.back())
    return Status{ErrorCode::InvalidArgument,
                  "invalid numeric implementation identity"};
  std::uint64_t sum = 0;
  for (const auto count : report.fallback_reasons) {
    if (count > UINT64_MAX - sum)
      return Status{ErrorCode::InvalidArgument,
                    "numeric fallback counter overflow"};
    sum += count;
  }
  if (sum != report.strict_fallbacks)
    return Status{ErrorCode::InvalidArgument,
                  "inconsistent numeric fallback counts"};
  auto merged = *target;
  if (merged.profile == CpuNumericProfile::Unspecified) {
    merged = report;
  } else {
    if (merged.profile != report.profile ||
        merged.implementation != report.implementation)
      return Status{ErrorCode::InvalidArgument,
                    "numeric implementation changed within attempt"};
    if (report.evaluated_values > UINT64_MAX - merged.evaluated_values ||
        report.strict_fallbacks > UINT64_MAX - merged.strict_fallbacks ||
        report.view_elements > UINT64_MAX - merged.view_elements ||
        report.copied_elements > UINT64_MAX - merged.copied_elements)
      return Status{ErrorCode::ResourceExhausted,
                    "numeric diagnostic counter overflow",
                    FailureReason::CapacityLimit};
    merged.evaluated_values += report.evaluated_values;
    merged.strict_fallbacks += report.strict_fallbacks;
    merged.view_elements += report.view_elements;
    merged.copied_elements += report.copied_elements;
    for (unsigned i = 0; i < merged.fallback_reasons.size(); ++i)
      merged.fallback_reasons[i] += report.fallback_reasons[i];
  }
  *target = merged;
  return Status::success();
}
}  // namespace ps
