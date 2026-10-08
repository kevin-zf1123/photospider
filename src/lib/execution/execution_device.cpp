#include "execution/execution_device.hpp"

#include <memory>
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif
namespace ps::execution_internal {
/** @brief Availability of the scheduling lane, independently of native
 * capabilities in the noninstalled scheduler test kernel. */
bool gpu_lane_available(ThreadPool* lane,
                        const std::shared_ptr<gpu_internal::Device>& device) {
  if (!lane)
    return false;
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  if (!device)
    return true;
#endif
  return device && device->available();
}

/** @brief Creates production native resources; scheduler fixtures use fake
 * lanes. */
std::shared_ptr<gpu_internal::Device> execution_device(
    bool enabled, const std::shared_ptr<ResourceBudget>& budget) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  // Scheduler fixtures default to hardware-independent lanes. Native lifetime
  // and cancellation fixtures explicitly opt into the production device.
  if (!execution_testing::use_native_device())
    return {};
#endif
  return enabled ? gpu_internal::Device::create(budget) : nullptr;
}
}  // namespace ps::execution_internal
