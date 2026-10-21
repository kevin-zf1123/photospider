#pragma once

#include <cstdint>

#include "photospider/core/status.hpp"

namespace ps::execution_testing {
/** @brief Private native allocation fault seams in the test-kernel variant.
 * The caller keeps this immutable set alive until clearing it and until all
 * native calls have returned. Competing installations are not supported.
 * No installed kernel or public ABI exposes these callbacks.
 */
struct ExecutionTestHooks final {
  /** @brief Vulkan allocation fault checkpoint; zero disables injection. */
  std::uint32_t native_allocation_failure = 0;
  /** @brief Optional smaller Vulkan allocation-object limit. */
  std::uint32_t native_allocation_limit = 0;
  /** @brief Observes vkFreeMemory before allocation capacity is returned. */
  void (*native_memory_freed)() noexcept = nullptr;
  /** @brief Injects a typed error before Metal/Vulkan capacity preparation. */
  ErrorCode (*native_capacity_error)() noexcept = nullptr;
};

/** @brief Install borrowed hooks, or clear them with null. */
void install_execution_test_hooks(const ExecutionTestHooks* hooks) noexcept;
bool fail_native_allocation(std::uint32_t checkpoint) noexcept;
std::uint32_t native_allocation_limit(std::uint32_t physical) noexcept;
void notify_native_memory_freed() noexcept;
ErrorCode native_capacity_error() noexcept;
}  // namespace ps::execution_testing
