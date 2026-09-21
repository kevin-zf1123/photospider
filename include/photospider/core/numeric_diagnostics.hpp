#pragma once

#include <array>
#include <cstdint>

#include "photospider/core/status.hpp"

namespace ps {
/** @brief Actual CPU arithmetic path, separate from the CPU/GPU scheduler.
 * Strict preserves each operator's specified reference and rounding boundaries.
 * Accelerated floating outputs permit at most four FP32 representable steps.
 * Float64 inputs/outputs retain their dtype; their direct absolute error is
 * bounded by 4*2^(floor(log2(abs(reference)))-23) in the normal FP32 range.
 * Zero, FP32-subnormal-range and out-of-FP32-range references use strict.
 * Special values, signed zero, integer/discrete results, copies and selected
 * endpoints preserve their exact operator rules. Monotonicity and other
 * operator invariants remain additional requirements; a per-value bound alone
 * does not establish them. Final expression output, not each isolated math
 * call, owns the numeric error budget. Caller floating state is preserved.
 */
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
/** @brief Bounded function attribution for expression mathematical calls.
 * Other retains reports from operations without expression-function
 * attribution.
 */
enum class NumericMathFunction : std::uint32_t {
  Other = 0,
  Sqrt = 1,
  Exp = 2,
  Ln = 3,
  Sin = 4,
  Cos = 5,
  Tan = 6,
  Pow = 7
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
  /** @brief Reported strict mathematical-call attempts, including failed calls.
   * Expression generators report every dispatched mathematical primitive;
   * Bezier function samplers report exact Bx sign evaluations in root
   * refinement. These counts exclude topology checks and interval/gcd work.
   * Operators without call-level instrumentation leave this field zero.
   */
  std::uint64_t strict_math_calls = 0;
  /** @brief Function/reason fallback counts, indexed by NumericMathFunction.
   * Unattributed per-reason counts are assigned to Other during merge. Known
   * attributions must not exceed their corresponding aggregate reason count.
   */
  std::array<std::array<std::uint64_t, 4>, 8> function_fallbacks{};
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
        report.strict_math_calls ||
        report.function_fallbacks !=
            std::array<std::array<std::uint64_t, 4>, 8>{} ||
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
  auto attributed = report;
  for (unsigned reason = 0; reason < 4; ++reason) {
    std::uint64_t count = 0;
    for (const auto& function : report.function_fallbacks) {
      if (function[reason] > UINT64_MAX - count)
        return Status{ErrorCode::InvalidArgument,
                      "function fallback counter overflow"};
      count += function[reason];
    }
    if (count > report.fallback_reasons[reason])
      return Status{ErrorCode::InvalidArgument,
                    "inconsistent function fallback counts"};
    attributed.function_fallbacks[0][reason] +=
        report.fallback_reasons[reason] - count;
  }
  auto merged = *target;
  if (merged.profile == CpuNumericProfile::Unspecified) {
    merged = attributed;
  } else {
    if (merged.profile != report.profile ||
        merged.implementation != report.implementation)
      return Status{ErrorCode::InvalidArgument,
                    "numeric implementation changed within attempt"};
    if (report.evaluated_values > UINT64_MAX - merged.evaluated_values ||
        report.strict_fallbacks > UINT64_MAX - merged.strict_fallbacks ||
        report.view_elements > UINT64_MAX - merged.view_elements ||
        report.copied_elements > UINT64_MAX - merged.copied_elements ||
        report.strict_math_calls > UINT64_MAX - merged.strict_math_calls)
      return Status{ErrorCode::ResourceExhausted,
                    "numeric diagnostic counter overflow",
                    FailureReason::CapacityLimit};
    for (unsigned function = 0; function < 8; ++function)
      for (unsigned reason = 0; reason < 4; ++reason)
        if (attributed.function_fallbacks[function][reason] >
            UINT64_MAX - merged.function_fallbacks[function][reason])
          return Status{ErrorCode::ResourceExhausted,
                        "numeric function counter overflow",
                        FailureReason::CapacityLimit};
    merged.strict_math_calls += report.strict_math_calls;
    for (unsigned function = 0; function < 8; ++function)
      for (unsigned reason = 0; reason < 4; ++reason)
        merged.function_fallbacks[function][reason] +=
            attributed.function_fallbacks[function][reason];
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
