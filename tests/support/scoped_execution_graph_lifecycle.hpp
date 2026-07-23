/**
 * @file scoped_execution_graph_lifecycle.hpp
 * @brief Declares explicit lifecycle registration for direct service tests.
 */
#pragma once

#include <exception>
#include <memory>

#include "compute/execution_service.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps::testing {

/**
 * @brief Registers one direct-test Graph with an ExecutionService for a scope.
 *
 * Kernel-owned product tests exercise registration through GraphRuntime load.
 * Lower-boundary ComputeService tests have no Kernel, so this helper models the
 * same explicit row lifetime without weakening production admission.
 *
 * @throws RunLifecycleRegistry registration exceptions from construction.
 * @note Declare this value after the matching GraphModel and ExecutionService.
 * Destruction closes the empty/settled row and marks its synthetic lifetime
 * anchor retired before either owner is destroyed.
 */
class ScopedExecutionGraphLifecycle final {
 public:
  /**
   * @brief Registers one preallocated lifetime anchor for the exact Graph.
   * @param service Stable direct-test execution service.
   * @param graph Stable Graph whose nonreused instance identity is registered.
   * @throws RunLifecycleRegistry registration exceptions unchanged.
   */
  ScopedExecutionGraphLifecycle(compute::ExecutionService& service,
                                const GraphModel& graph)
      : service_(service),
        anchor_(std::make_shared<compute::GraphLifetimeAnchor>(
            graph.instance_id())) {
    service_.register_graph_lifecycle(anchor_);
  }

  /**
   * @brief Settles the registered row before direct-test owner destruction.
   * @throws Nothing; an invariant or synchronization failure terminates.
   */
  ~ScopedExecutionGraphLifecycle() noexcept {
    try {
      service_.close_graph_lifecycle(
          anchor_->graph_instance_id(),
          compute::ComputeRunCancellationReason::GraphClose);
      anchor_->mark_retired();
    } catch (...) {
      std::terminate();
    }
  }

  /** @brief Prevents duplicate registration/close ownership. */
  ScopedExecutionGraphLifecycle(const ScopedExecutionGraphLifecycle&) = delete;

  /** @brief Prevents assigning duplicate registration/close ownership. */
  ScopedExecutionGraphLifecycle& operator=(
      const ScopedExecutionGraphLifecycle&) = delete;

 private:
  /** @brief Borrowed service outliving this scope guard. */
  compute::ExecutionService& service_;

  /** @brief Synthetic direct-test Graph lifetime/close record. */
  std::shared_ptr<compute::GraphLifetimeAnchor> anchor_;
};

}  // namespace ps::testing
