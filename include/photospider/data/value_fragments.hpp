#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/data/footprint.hpp"
#include "photospider/data/value.hpp"

namespace ps {
/** @brief Owned immutable fragments with exact authorized logical coverage.
 * @note Each Value is a complete rectangle. Holes never become valid pixels.
 * Copies retain shared storage owners; concurrent const reads are safe. This
 * carrier conveys bytes/metadata only: execution records must separately retain
 * producer, snapshot and effective-observation provenance. It grants no right
 * to reclassify a RequestRecord producer as an external atomic input.
 * Allocating methods may throw bad_alloc; no failed method publishes a result.
 */
class PHOTOSPIDER_API ValueFragments final {
 public:
  ValueFragments() = default;
  ValueFragments(const ValueFragments&) = default;
  ValueFragments(ValueFragments&&) noexcept = default;
  ValueFragments& operator=(const ValueFragments&);
  ValueFragments& operator=(ValueFragments&&) noexcept;
  /** @brief Clips supplied Values to authorization, checks exact completeness.
   * @param descriptor Full domain and dtype, independent of stored rectangles.
   * @param facets Canonical immutable metadata shared by every fragment.
   * @param authorized Exact permitted set, in the descriptor domain.
   * @param fragments Immutable owners; metadata must match. Overlaps are legal
   * only for identical storage and address mappings. Identical overlaps dedup.
   * Rectangular neighbors with the same owner/mapping may coalesce; returned
   * fragment count and partition need not equal the supplied partition.
   * @param limits Bounds normalization, including cancellation.
   * @param resources Facet resources, inferred from the first fragment when
   * omitted. Empty CMYK coverage still requires explicit owning resources.
   * @return Complete fragments or InvalidArgument/TypeMismatch/NotFound (hole),
   * ResourceExhausted/Cancelled. Typed images require full C in every fragment.
   */
  static Result<ValueFragments> create(ValueDescriptor descriptor,
                                       std::vector<ValueFacet> facets,
                                       Footprint authorized,
                                       const std::vector<Value>& fragments,
                                       const FootprintLimits& limits = {},
                                       ResourceBindings resources = {});
  /** @brief Same validation from a borrowed contiguous array, without a
   * temporary std::vector. A null pointer is valid only for count zero.
   * The array is borrowed for this call; returned fragments retain owners.
   * metadata_lifetime optionally retains a trusted host's publication capacity
   * until owned metadata storage is destroyed. It must not own this result or
   * its Values. This lifetime token does not account arbitrary caller copies.
   */
  static Result<ValueFragments> create_view(
      ValueDescriptor descriptor, std::vector<ValueFacet> facets,
      Footprint authorized, const Value* fragments, std::size_t count,
      const FootprintLimits& limits = {},
      std::shared_ptr<const void> metadata_lifetime = {},
      ResourceBindings resources = {});
  bool valid() const noexcept { return authorized_.valid(); }
  const ValueDescriptor& descriptor() const noexcept { return descriptor_; }
  const std::vector<ValueFacet>& facets() const noexcept { return facets_; }
  const ResourceBindings& resources() const noexcept { return resources_; }
  const Footprint& coverage() const noexcept { return authorized_; }
  const std::vector<Value>& fragments() const noexcept { return fragments_; }
  /** @brief Copies one authorized sample, with checked logical addressing.
   * @param coordinate Full rank logical coordinate.
   * @param destination Writable size-byte span, borrowed until return.
   * @param size Must equal the descriptor's scalar width, including Int64.
   * @return InvalidArgument outside authorization, TypeMismatch for size,
   * NotFound for a missing fragment; never supplies zero for a missing sample.
   * @note No upstream execute, allocation or implicit boundary extension.
   */
  Status read(const std::vector<std::uint64_t>& coordinate, void* destination,
              std::size_t size) const;
  /** @brief Restricts owners and authorization together; outside coverage
   * fails.
   */
  Result<ValueFragments> restrict(const Footprint& subset,
                                  const FootprintLimits& limits = {}) const;
  /** @brief Collects a complete authorized rectangle into host-allocated bytes.
   * @note Missing coverage fails before allocation; no hole filling. Copies
   * samples in logical order and observes cancellation before publication.
   */
  Result<Value> collect(const Region& region, const BufferAllocator& allocator,
                        const FootprintLimits& limits = {}) const;
  /** @brief Unique owner capacities, not just valid sample bytes.
   * @return Checked total; ResourceExhausted if capacities cannot be summed.
   */
  Result<std::uint64_t> retained_bytes() const;

 private:
  void swap(ValueFragments&) noexcept;
  std::shared_ptr<const void> metadata_lifetime_;
  ResourceBindings resources_;
  ValueDescriptor descriptor_;
  std::vector<ValueFacet> facets_;
  Footprint authorized_;
  std::vector<Value> fragments_;
};
}  // namespace ps
