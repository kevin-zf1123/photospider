#pragma once

#include <cstdint>

/**
 * @file device.hpp
 * @brief Stable device capability labels shared by public SDK contracts.
 */

namespace ps {

/**
 * @brief Identifies a compute or memory device without exposing native handles.
 *
 * @throws Nothing.
 * @note Values and the `uint32_t` representation are part of the public
 *       extension SDK contract. Capability labels do not grant access to a
 *       platform device, command queue, or mutable runtime owner.
 */
enum class Device : std::uint32_t {
  /** @brief Host CPU memory and execution. */
  CPU = 0U,

  /** @brief Apple Metal GPU memory and execution. */
  GPU_METAL = 1U,

  /** @brief CUDA GPU memory and execution. */
  GPU_CUDA = 2U,

  /** @brief Neural processing unit or ASIC execution. */
  ASIC_NPU = 3U,
};

}  // namespace ps
