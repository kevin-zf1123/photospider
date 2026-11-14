#include "photospider/data/device_storage.hpp"

#include <memory>

#include "photospider/core/resource_allocator.hpp"

namespace ps::gpu_internal {
std::shared_ptr<DeviceStorage> DeviceStorage::create(
    const ResourceBudget* root) {
  // The final weak release may unload the provider. Keep the control block
  // and its allocator in the data library through the end of deallocation.
  if (!root)
    return std::make_shared<DeviceStorage>();
  return std::allocate_shared<DeviceStorage>(
      ResourceAllocator<DeviceStorage>(*root));
}
}  // namespace ps::gpu_internal
