#include <memory>

#include "execution/metal_device_executor.hpp"

namespace ps::execution {

/** @copydoc make_default_metal_device_executor */
std::unique_ptr<DeviceExecutor> make_default_metal_device_executor() {
  return nullptr;
}

}  // namespace ps::execution
