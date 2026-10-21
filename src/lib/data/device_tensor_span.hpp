#pragma once

#include <memory>

#include "data/device_storage.hpp"
#include "photospider/data/tensor_span.hpp"

namespace ps::data_internal {
/** @brief Immutable affine metadata retaining opaque Device storage.
 * Queries return byte indexes only. This type never exposes CPU bytes.
 */
class DeviceTensorSpan final {
 public:
  static Result<DeviceTensorSpan> from_storage(
      const TensorDescriptor& descriptor, const Region& region,
      const StridedLayout& layout,
      std::shared_ptr<const gpu_internal::DeviceStorage> storage);
  Result<DeviceTensorSpan> view(const Region& region) const;
  Result<std::size_t> byte_address(const std::uint64_t* at,
                                   std::size_t rank) const;
  const Region& region() const;
  const StridedLayout& layout() const;
  const TensorDescriptor& descriptor() const;
  const std::shared_ptr<const gpu_internal::DeviceStorage>& storage() const {
    return storage_;
  }

 private:
  std::shared_ptr<const gpu_internal::DeviceStorage> storage_;
  struct Metadata;
  std::shared_ptr<const Metadata> metadata_;
};
}  // namespace ps::data_internal
