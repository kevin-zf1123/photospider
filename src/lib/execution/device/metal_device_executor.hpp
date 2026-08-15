#pragma once

#include <memory>

#include "execution/device/device_executor_registry.hpp"

/**
 * @file metal_device_executor.hpp
 * @brief Private platform factory for one process-owned Metal executor.
 */

namespace ps::execution {

/**
 * @brief Creates one usable process-owned Metal executor when available.
 * @return Native executor owner, or null when the platform/device/queue is
 * unavailable.
 * @param residency_manager Non-null replica publication authority shared with
 * the owning registry.
 * @throws std::bad_alloc when C++ executor allocation fails.
 * @throws std::invalid_argument when residency_manager is null.
 * @note The Apple implementation performs device/queue discovery; the neutral
 * implementation probes no native SDK and returns null.
 */
std::unique_ptr<DeviceExecutor> make_default_metal_device_executor(
    std::shared_ptr<ResidencyManager> residency_manager);

}  // namespace ps::execution
