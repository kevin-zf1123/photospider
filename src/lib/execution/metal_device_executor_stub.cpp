#include <memory>
#include <stdexcept>

#include "execution/metal_device_executor.hpp"

namespace ps::execution {

/** @copydoc make_default_metal_device_executor */
std::unique_ptr<DeviceExecutor> make_default_metal_device_executor(
    std::shared_ptr<ResidencyManager> residency_manager) {
  if (!residency_manager) {
    throw std::invalid_argument("Metal executor requires a residency manager.");
  }
  return nullptr;
}

}  // namespace ps::execution
