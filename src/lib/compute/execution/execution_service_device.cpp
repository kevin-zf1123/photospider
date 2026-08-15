#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "compute/execution/execution_service_internal.hpp"

/**
 * @file execution_service_device.cpp
 * @brief Owns device discovery and resident-value acquisition.
 */

namespace ps::compute {

using namespace execution_service_detail;  // NOLINT(build/namespaces)

/** @copydoc ExecutionService::available_devices */
std::vector<Device> ExecutionService::available_devices() const {
  return {Device::CPU};
}

/** @copydoc ExecutionService::available_devices(const std::string&) */
std::vector<Device> ExecutionService::available_devices(
    const std::string& execution_type) const {
  if (execution_type == "gpu_pipeline") {
    if (pool_->device_executors.contains(Device::GPU_METAL)) {
      return {Device::GPU_METAL, Device::CPU};
    }
    return {Device::CPU};
  }
  if (execution_type == "cpu" || execution_type == "serial_debug") {
    return {Device::CPU};
  }
  throw GraphError(GraphErrc::NotFound,
                   "Unknown private execution route: " + execution_type);
}

/** @copydoc ExecutionService::has_device_executor */
bool ExecutionService::has_device_executor(Device device) const noexcept {
  return pool_->device_executors.contains(device);
}

/** @copydoc ExecutionService::device_executor_diagnostics */
execution::DeviceExecutorDiagnostics
ExecutionService::device_executor_diagnostics(Device device) const {
  return pool_->device_executors.diagnostics(device);
}

/** @copydoc ExecutionService::acquire_metal_resident_value */
DeviceResidentValueAcquisition ExecutionService::acquire_metal_resident_value(
    Value source, std::uint32_t width, std::uint32_t height,
    const execution::DeviceCompletionSeed& completion_seed,
    std::chrono::steady_clock::time_point capture_deadline) {
  if (!source.valid() || width == 0U || height == 0U) {
    throw std::invalid_argument(
        "Metal residency acquisition requires a valid Value and positive "
        "dimensions.");
  }
  if (!pool_->device_executors.contains(Device::GPU_METAL)) {
    throw std::invalid_argument(
        "Metal residency acquisition requires a registered executor.");
  }
  if (std::chrono::steady_clock::now() >= capture_deadline) {
    throw std::runtime_error(
        "I2 Metal acquisition deadline expired before residency lookup.");
  }
  const ReadyFenceSnapshot source_state = source.ready_fence().poll();
  if (!source_state.ready()) {
    throw ReadyFenceAccessError(source_state);
  }

  const std::shared_ptr<execution::ResidencyManager> residency =
      pool_->device_executors.residency_manager();
  if (!residency) {
    throw std::logic_error(
        "Metal executor registry has no process residency manager.");
  }
  const DeviceId metal_device(DeviceBackend::Metal);
  std::optional<Value> resident = residency->find_published_value_acquisition(
      completion_seed, source, metal_device, MemoryDomain::DeviceLocal);
  if (resident.has_value()) {
    const std::optional<ReadyFenceSnapshot> resident_state =
        poll_i2_metal_resident_reuse(resident->ready_fence(), capture_deadline);
    if (!resident_state.has_value()) {
      throw std::runtime_error(
          "I2 Metal acquisition deadline expired before resident reuse.");
    }
    if (!resident_state->ready()) {
      throw ReadyFenceAccessError(*resident_state);
    }
    return DeviceResidentValueAcquisition{std::move(*resident), false};
  }

  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->stopping || pool_->shutdown_in_progress ||
        pool_->shutdown_complete) {
      throw std::runtime_error(
          "ExecutionService cannot acquire Metal residency during shutdown.");
    }
  }
  if (std::chrono::steady_clock::now() >= capture_deadline) {
    throw std::runtime_error(
        "I2 Metal acquisition deadline expired before native submission.");
  }
  HostToMetalValueInvocation invocation(source, width, height, completion_seed,
                                        capture_deadline, pool_->ledger);
  pool_->device_executors.execute(Device::GPU_METAL, invocation);
  Value pending = invocation.take_published_value();
  if (!pending.valid()) {
    throw std::logic_error(
        "Metal executor completed without a published resident Value.");
  }

  const std::optional<ReadyFenceSnapshot> terminal =
      wait_for_i2_metal_completion_until(pending.ready_fence(),
                                         capture_deadline);
  if (!terminal.has_value()) {
    const execution::DeviceCompletionIdentity identity(completion_seed, source,
                                                       pending);
    (void)contain_i2_timed_out_transfer(*residency, identity, pending);
    throw std::runtime_error(
        "I2 Metal residency acquisition reached the absolute capture "
        "deadline.");
  }
  if (!terminal->ready()) {
    throw ReadyFenceAccessError(*terminal);
  }
  resident = residency->find_published_value_acquisition(
      completion_seed, source, metal_device, MemoryDomain::DeviceLocal);
  if (!resident.has_value() ||
      resident->storage_binding() != pending.storage_binding()) {
    throw std::logic_error(
        "Metal completion did not publish the exact resident binding.");
  }
  return DeviceResidentValueAcquisition{std::move(*resident), true};
}

/** @copydoc ExecutionService::release_metal_resident_value */
bool ExecutionService::release_metal_resident_value(
    ValueRevisionId revision, const StorageBinding& binding,
    ProducerIdentity producer) {
  if (binding.device != DeviceId(DeviceBackend::Metal) ||
      binding.memory_domain != MemoryDomain::DeviceLocal) {
    return false;
  }
  const std::shared_ptr<execution::ResidencyManager> residency =
      pool_->device_executors.residency_manager();
  if (!residency) {
    throw std::logic_error(
        "Metal resident release requires the process residency manager.");
  }
  return residency->release_resident(revision, binding, producer);
}

}  // namespace ps::compute
