#pragma once

#include <cstdint>

#include "compute/run_lifecycle_registry.hpp"

namespace ps::testing {

/**
 * @brief Test-only access to lifecycle finalization wait fault injection.
 *
 * @note This private support header is compiled only by repository tests. It
 * exposes no Host, plugin, Policy ABI, or installed-library surface.
 */
class RunLifecycleRegistryTestAccess final {
 public:
  /**
   * @brief Callback at one registry finalization wait boundary.
   * @param context Opaque fixture state.
   * @param bundle_id Exact installed bundle.
   * @param resource_phase False for lease quiescence, true for root settlement.
   * @return Nothing.
   * @throws Test-selected synchronization exception unchanged.
   */
  using FinalizationWaitObserver = void (*)(void* context,
                                            std::uint64_t bundle_id,
                                            bool resource_phase);

  /**
   * @brief Installs one observer on an isolated registry.
   * @param registry Registry under deterministic test.
   * @param observer Callback, or null to disable.
   * @param context Opaque context, or null when disabling.
   * @return Nothing.
   * @throws Nothing.
   * @note Installation and clearing happen outside concurrent finalization.
   */
  static void set_finalization_wait_observer(
      compute::RunLifecycleRegistry& registry,
      FinalizationWaitObserver observer, void* context) noexcept {
    registry.finalization_wait_observer_context_ = context;
    registry.finalization_wait_observer_ = observer;
  }

  /**
   * @brief Clears one installed finalization observer.
   * @param registry Isolated registry whose observer is removed.
   * @return Nothing.
   * @throws Nothing.
   */
  static void clear_finalization_wait_observer(
      compute::RunLifecycleRegistry& registry) noexcept {
    registry.finalization_wait_observer_ = nullptr;
    registry.finalization_wait_observer_context_ = nullptr;
  }
};

}  // namespace ps::testing
