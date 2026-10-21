#pragma once

#include <memory>

#include "data/device_tensor_span.hpp"
#include "photospider/data/result.hpp"

namespace ps::data_internal {
/** @brief Coordinator access to Result's opaque Device backing.
 * Publication retains storage but does not seal it. An admission-time Result
 * envelope stays private until producer completion and ordered publication.
 */
class ResultDeviceAccess final {
 public:
  static Status publish(
      ResultBuilder& builder, std::uint32_t slot, const Region& region,
      const StridedLayout& layout,
      std::shared_ptr<const gpu_internal::DeviceStorage> storage,
      const CancellationToken& cancellation = {});
  /** @brief Authorize one rectangle backed by one Device allocation.
   * Captured coverage constrains access. Multiple allocations require explicit
   * COPY; the returned span retains only the selected allocation and metadata.
   */
  static Result<DeviceTensorSpan> acquire(
      const ResultRef& result, const ResultDescriptor& descriptor,
      std::uint32_t slot, const Region& region,
      const CancellationToken& cancellation = {});
  static Result<std::shared_ptr<const gpu_internal::DeviceStorage>> reference(
      const ResourceBudget& budget,
      std::shared_ptr<const gpu_internal::DeviceStorage> storage);
};
/** @brief Intersection of already validated rectangles of the same rank. */
inline bool regions_overlap(const Region& a, const Region& b) noexcept {
  if (a.rank() != b.rank() || a.empty() || b.empty())
    return false;
  for (std::size_t i = 0; i < a.rank(); ++i) {
    const auto x = a.dimensions()[i], y = b.dimensions()[i];
    if (x.offset >= y.offset + y.extent || y.offset >= x.offset + x.extent)
      return false;
  }
  return true;
}
}  // namespace ps::data_internal
