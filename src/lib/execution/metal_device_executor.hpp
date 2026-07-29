#pragma once

#include <memory>

#include "execution/device_executor_registry.hpp"

/**
 * @file metal_device_executor.hpp
 * @brief Private platform factory for one process-owned Metal executor.
 */

namespace ps::execution {

/**
 * @brief Creates one usable process-owned Metal executor when available.
 * @return Native executor owner, or null when the platform/device/queue is
 * unavailable.
 * @throws std::bad_alloc when C++ executor allocation fails.
 * @note The Apple implementation performs device/queue discovery; the neutral
 * implementation probes no native SDK and returns null.
 */
std::unique_ptr<DeviceExecutor> make_default_metal_device_executor();

}  // namespace ps::execution
