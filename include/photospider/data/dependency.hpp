#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/footprint.hpp"

namespace ps {
namespace dependency_internal {
struct MetadataOwner;
}
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
/** @brief One input axis copied from an observation axis or held in a fixed
 * half-open interval. A nonnegative observation_axis selects that axis plus
 * signed translation; -1 selects fixed and requires translation zero. Mapping
 * creation checks translated coordinates against each piece coverage. Repeated
 * selected axes within one map are prohibited.
 */
struct DependencyAxis final {
  std::int32_t observation_axis = -1;
  RegionDimension fixed{0, 1};
  std::int64_t translation = 0;
  bool operator==(const DependencyAxis& other) const noexcept {
    return observation_axis == other.observation_axis &&
           fixed.offset == other.fixed.offset &&
           fixed.extent == other.fixed.extent &&
           translation == other.translation;
  }
  bool operator<(const DependencyAxis& other) const noexcept {
    if (observation_axis != other.observation_axis)
      return observation_axis < other.observation_axis;
    if (fixed.offset != other.fixed.offset)
      return fixed.offset < other.fixed.offset;
    if (fixed.extent != other.fixed.extent)
      return fixed.extent < other.fixed.extent;
    return translation < other.translation;
  }
};
/** @brief Closed exact mapping to one port's samples and non-spatial tags.
 * axes is empty for tag-only support, otherwise has one entry per input axis.
 * Creation splits roles into canonical individual roles and normalizes tags.
 */
struct DependencyMappedNeed final {
  std::uint32_t port = 0;
  std::uint32_t roles = static_cast<std::uint32_t>(DependencyRole::Data);
  std::vector<DependencyAxis> axes;
  std::vector<DependencyTag> tags;
  bool operator==(const DependencyMappedNeed& other) const noexcept {
    return port == other.port && roles == other.roles && axes == other.axes &&
           tags == other.tags;
  }
};
/** @brief One successfully observed exact region and its static relation.
 * Coverage is in observation coordinates, not necessarily output samples.
 */
struct DependencyMapPiece final {
  Footprint coverage;
  std::vector<DependencyMappedNeed> inputs;
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
  /** @brief Deep copies owned metadata after admitting its new capacity.
   * Uses the current managed metadata scope, otherwise inherits source root.
   * Allocation failure throws bad_alloc; assignment leaves this unchanged.
   * Moves transfer capacity ownership and invalidate the moved-from storage.
   */
  DependencyCertificate(const DependencyCertificate&);
  DependencyCertificate& operator=(const DependencyCertificate&);
  DependencyCertificate(DependencyCertificate&&) noexcept = default;
  DependencyCertificate& operator=(DependencyCertificate&&) noexcept;
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
  /** @brief Validates a bounded piecewise axis-map certificate without
   * enumerating covered observations. Pieces must be disjoint and exactly
   * cover coverage. Missing observations remain unknown.
   */
  static Result<DependencyCertificate> create_mapped(
      std::string identity, Footprint coverage,
      std::vector<std::vector<std::uint64_t>> input_shapes,
      std::vector<DependencyMapPiece> pieces,
      const FootprintLimits& limits = {});
  bool valid() const noexcept { return coverage_.valid(); }
  const std::string& identity() const noexcept { return identity_; }
  const Footprint& coverage() const noexcept { return coverage_; }
  /** @brief Explicit row storage; throws logic_error for a mapped certificate.
   * Use backward, row or materialize for representation-neutral access.
   */
  const std::vector<AtomCertificate>& rows() const;
  bool mapped() const noexcept { return mapped_; }
  const std::vector<DependencyMapPiece>& mapping_pieces() const noexcept {
    return pieces_;
  }
  /** @brief Bounded metadata/work units, independent of mapped cardinality. */
  std::uint64_t metadata_entries() const noexcept { return metadata_entries_; }
  /** @brief Cache storage units including every retained coordinate, shape,
   * tag and repeated row support. Projection deduplication does not reduce
   * this cost. UINT64_MAX means the storage count cannot be represented.
   */
  std::uint64_t storage_entries() const noexcept { return storage_entries_; }
  /** @brief Copies a certificate with a new nonempty bounded identity. */
  Result<DependencyCertificate> with_identity(
      std::string identity, const FootprintLimits& limits = {}) const;
  /** @brief Exact one-observation support, rejecting unknown coordinates. */
  Result<AtomCertificate> row(const std::vector<std::uint64_t>& coordinate,
                              const FootprintLimits& limits = {}) const;
  /** @brief Explicitly materializes rows within caller limits. No implicit
   * expansion occurs in backward, transpose, restriction or matching-map merge.
   */
  Result<std::vector<AtomCertificate>> materialize(
      const FootprintLimits& limits = {}) const;
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
  std::shared_ptr<const dependency_internal::MetadataOwner> metadata_owner_;
  std::string identity_;
  Footprint coverage_;
  std::vector<std::vector<std::uint64_t>> input_shapes_;
  std::vector<AtomCertificate> rows_;
  bool mapped_ = false;
  std::vector<DependencyMapPiece> pieces_;
  std::uint64_t metadata_entries_ = 0;
  std::uint64_t storage_entries_ = 0;
  void swap(DependencyCertificate&) noexcept;
  DependencyCertificate(const DependencyCertificate&, std::string identity);
  static Result<DependencyCertificate> create_owned(
      std::string identity, Footprint coverage,
      std::vector<std::vector<std::uint64_t>> shapes,
      std::vector<AtomCertificate> rows, const FootprintLimits& limits,
      const std::shared_ptr<const dependency_internal::MetadataOwner>& source);
  static Result<DependencyCertificate> create_mapped_owned(
      std::string identity, Footprint coverage,
      std::vector<std::vector<std::uint64_t>> shapes,
      std::vector<DependencyMapPiece> pieces, const FootprintLimits& limits,
      const std::shared_ptr<const dependency_internal::MetadataOwner>& source);
  std::uint64_t measure_storage() const noexcept;
  Result<DependencyCertificate> restrict_mapped(const Footprint&,
                                                const FootprintLimits&) const;
  Result<std::vector<DependencyNeed>> backward_mapped(
      const Footprint&, const FootprintLimits&) const;
  Result<Footprint> transpose_mapped(const DependencyNeed&,
                                     const FootprintLimits&) const;
  Result<DependencyCertificate> merge_mapped(const DependencyCertificate&,
                                             const FootprintLimits&) const;
};
}  // namespace ps
