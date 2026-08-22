#pragma once

#include <cstdint>
#include <stdexcept>

/**
 * @file device.hpp
 * @brief Stable device capability labels shared by public SDK contracts.
 */

namespace ps {

/**
 * @brief Stable backend family independent from a concrete device instance.
 *
 * @throws Nothing for ordinary value operations.
 * @note A backend family grants no native handle, queue, memory access, or
 *       execution capability. Operation selection and memory contracts both
 *       use this family while concrete placement uses `DeviceId`.
 */
enum class DeviceBackend : std::uint32_t {
  /** @brief Host CPU backend family. */
  CPU = 0U,
  /** @brief Apple Metal backend family. */
  Metal = 1U,
  /** @brief NVIDIA CUDA backend family. */
  CUDA = 2U,
  /** @brief Vulkan compute backend family. */
  Vulkan = 3U,
  /** @brief Neural-processing or ASIC backend family. */
  NPU = 4U,
};

/**
 * @brief Version-one allocation memory-domain classification.
 *
 * @throws Nothing for ordinary value operations.
 * @note A memory domain is a storage fact, not a statement that a caller can
 *       map, dereference, import, or observe the allocation coherently.
 */
enum class MemoryDomain : std::uint32_t {
  /** @brief Ordinary host allocation. */
  Host = 0U,
  /** @brief Host-visible allocation prepared for device transfer. */
  HostPinned = 1U,
  /** @brief Device-local allocation without implied host visibility. */
  DeviceLocal = 2U,
  /** @brief Backend-managed shared allocation with explicit visibility work. */
  Shared = 3U,
  /** @brief Allocation imported from another owner or API. */
  Imported = 4U,
};

/**
 * @brief Checked process-local identity for one concrete backend device.
 *
 * @throws std::invalid_argument when constructed with an unknown backend.
 * @note The ordinal has meaning only within the live process and matching
 *       backend registry. It is not persistent and grants no device access.
 */
class DeviceId final {
 public:
  /**
   * @brief Constructs one checked concrete device identity.
   * @param backend Stable backend family.
   * @param ordinal Process-local backend ordinal.
   * @throws std::invalid_argument for an unknown backend enum value.
   */
  explicit constexpr DeviceId(DeviceBackend backend, std::uint32_t ordinal = 0U)
      : backend_(backend), ordinal_(ordinal) {
    switch (backend_) {
      case DeviceBackend::CPU:
      case DeviceBackend::Metal:
      case DeviceBackend::CUDA:
      case DeviceBackend::Vulkan:
      case DeviceBackend::NPU:
        return;
    }
    throw std::invalid_argument("DeviceId requires a known backend family.");
  }

  /**
   * @brief Returns the backend family.
   * @return Stable backend label.
   * @throws Nothing.
   */
  constexpr DeviceBackend backend() const noexcept { return backend_; }

  /**
   * @brief Returns the process-local backend ordinal.
   * @return Concrete device ordinal.
   * @throws Nothing.
   */
  constexpr std::uint32_t ordinal() const noexcept { return ordinal_; }

  /**
   * @brief Compares complete concrete device identity.
   * @param other Identity to compare.
   * @return True only when backend and ordinal match.
   * @throws Nothing.
   */
  constexpr bool operator==(const DeviceId& other) const noexcept {
    return backend_ == other.backend_ && ordinal_ == other.ordinal_;
  }

  /**
   * @brief Compares concrete device identity for inequality.
   * @param other Identity to compare.
   * @return True when backend or ordinal differs.
   * @throws Nothing.
   */
  constexpr bool operator!=(const DeviceId& other) const noexcept {
    return !(*this == other);
  }

  /**
   * @brief Orders device identities for deterministic private indexes.
   * @param other Identity to compare.
   * @return Lexicographic backend/ordinal order.
   * @throws Nothing.
   */
  constexpr bool operator<(const DeviceId& other) const noexcept {
    return backend_ < other.backend_ ||
           (backend_ == other.backend_ && ordinal_ < other.ordinal_);
  }

 private:
  /** @brief Stable backend family. */
  DeviceBackend backend_ = DeviceBackend::CPU;
  /** @brief Process-local concrete-device ordinal. */
  std::uint32_t ordinal_ = 0U;
};

}  // namespace ps
