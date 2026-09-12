#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "photospider/data/storage.hpp"
#include "photospider/execution/cancellation.hpp"

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
 */
class PHOTOSPIDER_API QualityReport final {
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
  /** @brief Only present when the named integer proof was checked. */
  std::optional<double> error_bound() const noexcept;
  /** @brief Exact proof inputs (diagonal, estimate, rhs) for independent
   * checks. Measured reports have no proof rows. No I/O or producer evaluation
   * occurs.
   */
  std::uint64_t proof_count() const noexcept;
  Result<std::array<std::int64_t, 3>> proof_row(std::uint64_t row) const;
  /** @brief Finite nonnegative computed residual, with no error bound. */
  static Result<QualityReport> measured_residual(
      std::string_view snapshot, std::uint64_t dimension, double residual,
      const BufferAllocator& allocator);
  /** @brief Checks an exact diagonal integer proof in the infinity norm.
   * count is 1..4096; each input magnitude <=2^26 and every diagonal is
   * nonzero. Integer residual arithmetic is exact (<2^53). R=max|a*x-b| and
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
