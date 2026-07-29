#include "execution/device_executor_registry.hpp"

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "execution/metal_device_executor.hpp"

namespace ps::execution {

/** @copydoc DeviceExecutorRegistry::slot_for */
std::size_t DeviceExecutorRegistry::slot_for(Device device) noexcept {
  switch (device) {
    case Device::CPU:
      return 0U;
    case Device::GPU_METAL:
      return 1U;
    case Device::GPU_CUDA:
      return 2U;
    case Device::ASIC_NPU:
      return 3U;
  }
  return kDeviceSlotCount;
}

/** @copydoc DeviceExecutorRegistry::register_executor */
void DeviceExecutorRegistry::register_executor(
    std::unique_ptr<DeviceExecutor> executor) {
  if (!executor) {
    throw std::invalid_argument(
        "DeviceExecutorRegistry requires a non-null executor.");
  }
  const Device device = executor->device();
  const std::size_t slot = slot_for(device);
  if (device == Device::CPU || slot >= kDeviceSlotCount) {
    throw std::invalid_argument(
        "DeviceExecutorRegistry accepts only known non-CPU devices.");
  }
  if (executors_[slot]) {
    throw std::invalid_argument(
        "DeviceExecutorRegistry rejects duplicate device registration.");
  }
  executors_[slot] = std::move(executor);
}

/** @copydoc DeviceExecutorRegistry::contains */
bool DeviceExecutorRegistry::contains(Device device) const noexcept {
  const std::size_t slot = slot_for(device);
  return device != Device::CPU && slot < kDeviceSlotCount &&
         executors_[slot] != nullptr;
}

/** @copydoc DeviceExecutorRegistry::size */
std::size_t DeviceExecutorRegistry::size() const noexcept {
  std::size_t result = 0U;
  for (std::size_t slot = 1U; slot < executors_.size(); ++slot) {
    if (executors_[slot]) {
      ++result;
    }
  }
  return result;
}

/** @copydoc DeviceExecutorRegistry::available_devices */
std::vector<Device> DeviceExecutorRegistry::available_devices() const {
  std::vector<Device> result;
  result.reserve(size());
  for (Device device :
       {Device::GPU_METAL, Device::GPU_CUDA, Device::ASIC_NPU}) {
    if (contains(device)) {
      result.push_back(device);
    }
  }
  return result;
}

/** @copydoc DeviceExecutorRegistry::execute */
void DeviceExecutorRegistry::execute(Device device,
                                     DeviceExecutorInvocation& invocation) {
  const std::size_t slot = slot_for(device);
  if (device == Device::CPU || slot >= kDeviceSlotCount || !executors_[slot]) {
    throw std::invalid_argument(
        "DeviceExecutorRegistry has no matching device executor.");
  }
  executors_[slot]->execute(invocation);
}

/** @copydoc DeviceExecutorRegistry::diagnostics */
DeviceExecutorDiagnostics DeviceExecutorRegistry::diagnostics(
    Device device) const {
  const std::size_t slot = slot_for(device);
  if (device == Device::CPU || slot >= kDeviceSlotCount || !executors_[slot]) {
    throw std::invalid_argument(
        "DeviceExecutorRegistry has no matching device executor.");
  }
  return executors_[slot]->diagnostics();
}

/** @copydoc make_default_device_executor_registry */
DeviceExecutorRegistry make_default_device_executor_registry() {
  DeviceExecutorRegistry registry;
  std::unique_ptr<DeviceExecutor> metal = make_default_metal_device_executor();
  if (metal) {
    registry.register_executor(std::move(metal));
  }
  return registry;
}

}  // namespace ps::execution
