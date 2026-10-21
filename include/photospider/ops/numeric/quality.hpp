#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "photospider/core/cancellation.hpp"
#include "photospider/data/storage.hpp"
#include "photospider/ops/export.hpp"

namespace ps {
/** @brief Numerical evidence strength, independent of dependency guarantees. */
enum class QualityEvidence : std::uint32_t {
  None = 0,
  Measured = 1,
  CertifiedBound = 2
};
/** @brief Immutable infinity-norm residual report with explicit evidence.
 * Only named factories can create reports; finite residual observations never
 * acquire a CertifiedBound tag by setting a caller-provided enum or tolerance.
 * The report's owned CPU allocation retains the supplied allocator/root.
 * This is a numerical bound, never a resource/RSS bound or dependency proof.
 * Runtime input structure and numerical conditions are unchecked caller
 * premises. Factories construct reports without input or publication scans.
 * snapshot contains 1..256 printable non-space ASCII bytes; dimension is
 * positive. Inputs, pointers and callbacks remain valid for the call.
 */
class PHOTOSPIDER_OPS_SDK_API QualityReport final {
 public:
  QualityReport() = default;
  bool valid() const noexcept { return storage_ != nullptr; }
  bool owned_by(const BufferAllocator& allocator) const noexcept;
  QualityEvidence evidence() const noexcept;
  /** @brief Nonempty caller-specified immutable system/domain snapshot name. */
  std::string_view snapshot() const noexcept;
  std::uint64_t dimension() const noexcept;
  /** @brief Infinity norm observation; zero for an invalid default handle. */
  double residual() const noexcept;
  /** @brief Only present after the named integer bound calculation. */
  std::optional<double> error_bound() const noexcept;
  /** @brief Exact proof inputs (diagonal, estimate, rhs) for independent
   * inspection. Measured reports have no proof rows. row<proof_count() is an
   * unchecked premise for proof_row. No I/O or producer evaluation
   * occurs.
   */
  std::uint64_t proof_count() const noexcept;
  Result<std::array<std::int64_t, 3>> proof_row(std::uint64_t row) const;
  /** @brief Record a supplied finite nonnegative residual, unchecked, with no
   * bound. */
  static Result<QualityReport> measured_residual(
      std::string_view snapshot, std::uint64_t dimension, double residual,
      const BufferAllocator& allocator);
  /** @brief Compute an exact diagonal integer error bound in the infinity norm.
   * Unchecked premises: count is 1..4096; arrays contain count entries;
   * consume_work is nonempty; each input magnitude <=2^26 and every diagonal
   * is nonzero. Integer residual arithmetic is exact (<2^53). R=max|a*x-b| and
   * m=min|a| give ||x-x*||inf<=R/m. The positive quotient is rounded upward
   * with one nextafter after nearest binary64 division; R=0 gives exact bound
   * zero. All proof inputs are retained in the admitted report allocation. Each
   * row is charged through consume_work before access; cancellation/work
   * failure returns no report. This does not certify arbitrary Float64 systems.
   */
  static Result<QualityReport> certify_integer_diagonal(
      std::string_view snapshot, const std::int64_t* diagonal,
      const std::int64_t* estimate, const std::int64_t* rhs,
      std::uint64_t count, const BufferAllocator& allocator,
      const std::function<Status(std::uint64_t)>& consume_work,
      const CancellationToken& cancellation = {});

 private:
  std::shared_ptr<const CpuStorage> storage_;
};
}  // namespace ps
