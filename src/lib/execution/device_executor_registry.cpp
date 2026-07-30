#include "execution/device_executor_registry.hpp"

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "execution/metal_device_executor.hpp"

namespace ps::execution {
namespace {

/** @brief Stable diagnostic for synchronous same-executor callback re-entry. */
constexpr char kReentryError[] = "Same-executor callback re-entry denied.";

/**
 * @brief One active executor callback frame on the current thread.
 *
 * @throws Nothing for aggregate construction and destruction.
 * @note The frame borrows executor identity only while `execute()` is active.
 */
struct ActiveExecutorCallbackFrame final {
  /** @brief Exact concrete executor identity represented by this frame. */
  const DeviceExecutor* executor = nullptr;

  /** @brief Previously active callback frame on the same thread. */
  ActiveExecutorCallbackFrame* previous = nullptr;
};

/**
 * @brief Innermost active device-executor callback on this thread.
 *
 * @note The intrusive stack allocates no storage and permits different
 * executor identities to nest while detecting A-to-B-to-A recursion.
 */
thread_local ActiveExecutorCallbackFrame* tls_callback_top = nullptr;

/**
 * @brief Owns one exact-executor callback identity on the current thread.
 *
 * Construction scans the active intrusive stack before publishing its frame.
 * Exact identity repetition fails before concrete executor admission. Normal
 * return and exception unwinding restore the previous frame in strict LIFO
 * order.
 *
 * @throws std::logic_error when the same executor is already active on this
 * thread.
 * @note The guard is thread-affine, allocation-free, and borrows the executor
 * only for the surrounding synchronous `DeviceExecutor::execute()` call.
 */
class ScopedExecutorCallbackIdentity final {
 public:
  /**
   * @brief Validates and publishes one executor callback identity.
   * @param executor Live executor entered by the surrounding call.
   * @throws std::logic_error with a stable message when this exact executor is
   * already active on the current thread.
   */
  explicit ScopedExecutorCallbackIdentity(const DeviceExecutor& executor)
      : frame_{&executor, tls_callback_top} {
    for (const ActiveExecutorCallbackFrame* active = tls_callback_top;
         active != nullptr; active = active->previous) {
      if (active->executor == &executor) {
        throw std::logic_error(kReentryError);
      }
    }
    tls_callback_top = &frame_;
  }

  /**
   * @brief Restores the previously active executor callback identity.
   * @throws Nothing; a broken LIFO invariant terminates the process.
   */
  ~ScopedExecutorCallbackIdentity() noexcept {
    if (tls_callback_top != &frame_) {
      std::terminate();
    }
    tls_callback_top = frame_.previous;
  }

  /**
   * @brief Prevents duplicating one thread-local restoration obligation.
   * @param other Unused source because copying is forbidden.
   * @throws Nothing because this operation is deleted.
   */
  ScopedExecutorCallbackIdentity(const ScopedExecutorCallbackIdentity& other) =
      delete;

  /**
   * @brief Prevents replacing one thread-local restoration obligation.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing because this operation is deleted.
   */
  ScopedExecutorCallbackIdentity& operator=(
      const ScopedExecutorCallbackIdentity& other) = delete;

 private:
  /** @brief Intrusive frame published for this guard's lexical lifetime. */
  ActiveExecutorCallbackFrame frame_;
};

}  // namespace

/** @copydoc DeviceExecutor::execute */
void DeviceExecutor::execute(DeviceExecutorInvocation& invocation) {
  ScopedExecutorCallbackIdentity callback_identity(*this);
  execute_impl(invocation);
}

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
