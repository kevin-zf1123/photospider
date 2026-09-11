#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "photospider/data/footprint.hpp"

namespace ps {
/** @brief Composable purposes of a declared input dependency. */
enum class DependencyRole : std::uint32_t {
  Data = 1,
  Control = 2,
  Validation = 4,
  Descriptor = 8
};
/** @brief Non-spatial input atom, e.g. descriptor or immutable index token.
 * @note Kind/id are contract-owned logical identifiers, never memory addresses.
 */
struct DependencyTag final {
  std::uint32_t kind = 0;
  std::uint64_t id = 0;
  bool operator==(const DependencyTag& other) const noexcept {
    return kind == other.kind && id == other.id;
  }
  bool operator<(const DependencyTag& other) const noexcept {
    return kind < other.kind || (kind == other.kind && id < other.id);
  }
};
/** @brief One port's spatial and tagged support, with explicit dependency
 * roles.
 * @note A NeedBatch is a fetch union of these sets; it is not a certificate.
 */
struct DependencyNeed final {
  std::uint32_t port = 0;
  std::uint32_t roles = static_cast<std::uint32_t>(DependencyRole::Data);
  Footprint samples;
  std::vector<DependencyTag> tags;
};
/** @brief Complete support for one output observation atom, including empty.
 * @note Generic atoms are logical samples. Image-v2 atoms are complete pixels
 * in an HW observation domain; channel closure belongs to input samples.
 */
struct AtomCertificate final {
  std::vector<std::uint64_t> output;
  std::vector<DependencyNeed> inputs;
};
/** @brief Immutable exact per-output association in one fixed
 * contract/snapshot.
 * @note Only successful, complete resolution creates a certificate. Missing
 * rows are unknown, never empty. RequestRecord/failure witnesses use a separate
 * execution path and cannot be represented as atomic success certificates.
 * Const access is concurrent-safe. Allocating methods may throw bad_alloc.
 * Limits reject with ResourceExhausted, cancellation with Cancelled; invalid
 * domains, rows or incompatible identities reject with InvalidArgument.
 * Row/edge copies and transpose output growth are bounded before allocation;
 * traversal work includes rows with no matching dirty role.
 */
class PHOTOSPIDER_API DependencyCertificate final {
 public:
  DependencyCertificate() = default;
  /** @brief Validates complete coverage and copies canonical rows.
   * @param identity Contract/numeric/error/snapshot identity, 1..4096 bytes.
   * @param coverage Output observation domain and exact resolved observations.
   * @param input_shapes Full logical input domains in declared port order.
   * @param rows Exactly one row per covered atom; duplicate rows are rejected.
   * @param limits Bounds rows/edges/tags and exact set operations.
   */
  static Result<DependencyCertificate> create(
      std::string identity, Footprint coverage,
      std::vector<std::vector<std::uint64_t>> input_shapes,
      std::vector<AtomCertificate> rows, const FootprintLimits& limits = {});
  bool valid() const noexcept { return coverage_.valid(); }
  const std::string& identity() const noexcept { return identity_; }
  const Footprint& coverage() const noexcept { return coverage_; }
  const std::vector<AtomCertificate>& rows() const noexcept { return rows_; }
  const std::vector<std::vector<std::uint64_t>>& input_shapes() const noexcept {
    return input_shapes_;
  }
  /** @brief Exact restriction; any unknown requested row rejects. */
  Result<DependencyCertificate> restrict(
      const Footprint& subset, const FootprintLimits& limits = {}) const;
  /** @brief Fetch projection only, grouped by port and individual role. */
  Result<std::vector<DependencyNeed>> backward(
      const Footprint& subset, const FootprintLimits& limits = {}) const;
  /** @brief Returns covered observations whose same-port support meets dirty.
   * @note Spatial and tagged atoms participate; a zero role intersection does
   * not dirty an observation. The returned set is exact only for coverage().
   */
  Result<Footprint> transpose(const DependencyNeed& dirty,
                              const FootprintLimits& limits = {}) const;
  /** @brief Merges matching identity/domain certificates; overlap rows must
   * agree.
   */
  Result<DependencyCertificate> merge(const DependencyCertificate& other,
                                      const FootprintLimits& limits = {}) const;

 private:
  std::string identity_;
  Footprint coverage_;
  std::vector<std::vector<std::uint64_t>> input_shapes_;
  std::vector<AtomCertificate> rows_;
};
}  // namespace ps
