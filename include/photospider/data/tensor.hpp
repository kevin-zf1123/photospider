#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/footprint.hpp"
#include "photospider/data/tensor_span.hpp"

namespace ps {
namespace data_internal {
class TensorAccess;
}
/** @brief Owned immutable fragments with exact authorized logical coverage.
 * @note Each TensorSpan is a complete rectangle. Holes never become valid
 * pixels. Copies retain shared storage owners; concurrent const reads are safe.
 * This carrier conveys bytes/metadata only: execution records must separately
 * retain producer and snapshot provenance. Allocating methods may throw
 * bad_alloc; no failed method publishes a result.
 */
class PHOTOSPIDER_API Tensor final {
 public:
  Tensor() = default;
  Tensor(const Tensor&) = default;
  Tensor(Tensor&&) noexcept = default;
  Tensor& operator=(const Tensor&);
  Tensor& operator=(Tensor&&) noexcept;
  /** @brief Clips supplied Tensor spans to authorization, checks exact
   * completeness.
   * @param descriptor Full domain and dtype, independent of stored rectangles.
   * @param authorized Exact permitted set, in the descriptor domain.
   * @param fragments Immutable owners; metadata must match. Overlaps are legal
   * only for identical storage and address mappings. Identical overlaps dedup.
   * Rectangular neighbors with the same owner/mapping may coalesce; returned
   * fragment count and partition need not equal the supplied partition.
   * @param limits Bounds normalization, including cancellation.
   * @return Complete fragments or InvalidArgument/TypeMismatch/NotFound (hole),
   * ResourceExhausted/Cancelled.
   */
  static Result<Tensor> create(TensorDescriptor descriptor,
                               Footprint authorized,
                               const std::vector<TensorSpan>& fragments,
                               const FootprintLimits& limits = {});
  /** @brief Same validation from a borrowed contiguous array, without a
   * temporary std::vector. A null pointer is valid only for count zero.
   * The array is borrowed for this call; returned fragments retain owners.
   * metadata_lifetime optionally retains a trusted host's publication capacity
   * until owned metadata storage is destroyed. It must not own this result or
   * its spans. This lifetime token does not account arbitrary caller copies.
   */
  static Result<Tensor> create_view(
      TensorDescriptor descriptor, Footprint authorized,
      const TensorSpan* fragments, std::size_t count,
      const FootprintLimits& limits = {},
      std::shared_ptr<const void> metadata_lifetime = {});
  bool valid() const noexcept { return authorized_.valid(); }
  const TensorDescriptor& descriptor() const noexcept { return descriptor_; }
  const Footprint& coverage() const noexcept { return authorized_; }
  /** @brief Immutable directory; invalid and moved-from tensors return empty.
   */
  const ResourceVector<TensorSpan>& fragments() const {
    static const ResourceVector<TensorSpan> empty;
    return storage_ ? storage_->fragments : empty;
  }
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
  /** @brief Authorized scalar read with bounded lookup work and cancellation.
   * Checks limits before each authorization box, fragment candidate and final
   * address/copy. Charges at most 2+4*rank+(boxes+fragments)*(rank+1) units;
   * maximum_work applies to this read, consume_work can enforce a shared
   * budget. Failure leaves destination unchanged. The limits and callback are
   * borrowed only for this synchronous call. No payload allocation or upstream
   * execution occurs. Status categories match read(), plus
   * ResourceExhausted/Cancelled or the original consume_work failure. Callback
   * exceptions propagate.
   */
  Status read(const std::vector<std::uint64_t>& coordinate, void* destination,
              std::size_t size, const FootprintLimits& limits) const;
  /** @brief Restricts owners and authorization together; outside coverage
   * fails.
   */
  Result<Tensor> restrict(const Footprint& subset,
                          const FootprintLimits& limits = {}) const;
  /** @brief Collects a complete authorized rectangle into host-allocated bytes.
   * @note Missing coverage fails before allocation; no hole filling. Copies
   * samples in logical order and observes cancellation before publication.
   * A packed rectangle from one fragment uses bounded bulk copies; general
   * layouts use checked sample reads. Both retain logical-sample work limits,
   * fresh allocator-owned storage.
   */
  Result<TensorSpan> collect(const Region& region,
                             const BufferAllocator& allocator,
                             const FootprintLimits& limits = {}) const;
  /** @brief Unique owner capacities, not just valid sample bytes.
   * Temporary owner metadata uses the fragment directory allocator.
   * Limits bound candidate traversal and cancellation; callbacks are borrowed
   * for this call. Returns ResourceExhausted on work, capacity or sum overflow.
   */
  Result<std::uint64_t> retained_bytes(
      const FootprintLimits& limits = {}) const;

 private:
  void swap(Tensor&) noexcept;
  std::shared_ptr<const void> metadata_lifetime_;
  TensorDescriptor descriptor_;
  Footprint authorized_;
  friend class data_internal::TensorAccess;
  struct Storage {
    ResourceVector<TensorSpan> fragments;
  };
  std::shared_ptr<Storage> storage_;
};
}  // namespace ps
