#pragma once

#include "compute/compute_run.hpp"

namespace ps::testing {

/**
 * @brief Test-only access to post-linearization cancellation fault injection.
 *
 * @note This private support header is repository-test-only and exposes no
 * installed API, Host surface, plugin contract, or cancellation authority.
 */
class ComputeRequestCancellationSourceTestAccess final {
 public:
  /**
   * @brief Callback invoked after the request reason becomes irreversible.
   * @param context Opaque test context.
   * @return Nothing.
   * @throws Test-selected synchronization exception.
   */
  using AfterLinearizationObserver = void (*)(void* context);

  /**
   * @brief Installs one observer on an isolated request source.
   * @param source Source used only by the owning repository test.
   * @param observer Callback, or null to disable.
   * @param context Opaque context, or null when disabling.
   * @return Nothing.
   * @throws Nothing.
   */
  static void set_after_linearization_observer(
      compute::ComputeRequestCancellationSource& source,
      AfterLinearizationObserver observer, void* context) noexcept {
    source.after_linearization_observer_context_ = context;
    source.after_linearization_observer_ = observer;
  }
};

}  // namespace ps::testing
