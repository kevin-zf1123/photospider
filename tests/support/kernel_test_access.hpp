#pragma once

#include <future>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "runtime/kernel.hpp"

namespace ps::testing {

/**
 * @brief Internal-only access bridge for tests that must inspect Kernel state.
 *
 * KernelTestAccess centralizes test-only runtime and graph-state access after
 * Kernel removed its runtime/post escape hatches. Production frontends call
 * public `ps::Host`; only the embedded Host adapter may translate those calls
 * to internal Kernel and InteractionService value facades. This header is only
 * included by test targets through the tests/support include path.
 *
 * @note Methods throw std::runtime_error for missing graphs so tests fail
 * loudly instead of silently treating a missing internal runtime as success.
 */
class KernelTestAccess {
 public:
  /**
   * @brief Injects resource exhaustion through Kernel::with_runtime().
   *
   * @param kernel Kernel whose mutable runtime helper is exercised.
   * @param name Loaded graph name used to resolve the runtime.
   * @return The helper result if the injected exception is incorrectly
   *         converted; the correct contract propagates std::bad_alloc.
   * @throws std::bad_alloc unconditionally after a loaded runtime is resolved.
   * @note This test-only seam exercises the real private wrapper without
   *       exposing it to production callers or frontends.
   */
  static std::optional<int> inject_bad_alloc_through_runtime(
      Kernel& kernel, const std::string& name) {
    return kernel.with_runtime(
        name, [](GraphRuntime&) -> int { throw std::bad_alloc{}; });
  }

  /**
   * @brief Injects resource exhaustion through Kernel::with_graph_state().
   *
   * @param kernel Kernel whose serialized graph-state helper is exercised.
   * @param name Loaded graph name used to resolve the executor.
   * @return The helper result if the injected exception is incorrectly
   *         converted; the correct contract propagates std::bad_alloc.
   * @throws std::bad_alloc through the GraphStateExecutor future.
   * @note The callable runs on the real graph-state executor rather than a
   *       mocked future or exception translator.
   */
  static std::optional<int> inject_bad_alloc_through_graph_state(
      Kernel& kernel, const std::string& name) {
    return kernel.with_graph_state(
        name, [](GraphModel&) -> int { throw std::bad_alloc{}; });
  }

  /**
   * @brief Injects resource exhaustion through the reload LastError wrapper.
   *
   * @param kernel Kernel whose LastError-producing graph-state helper runs.
   * @param name Loaded graph name used to resolve the executor.
   * @return The helper result if resource exhaustion is incorrectly recorded
   *         as LastError; the correct contract propagates std::bad_alloc.
   * @throws std::bad_alloc through the submitted graph-state future.
   * @note `reload_graph_document()` uses this exact private wrapper; the
   * injection isolates its catch ordering without adding a production debug
   * hook.
   */
  static std::optional<int> inject_bad_alloc_through_last_error_graph_state(
      Kernel& kernel, const std::string& name) {
    return kernel.with_graph_state_last_error(
        name, "injected reload failure: ", [](GraphModel&) -> int {
          throw std::bad_alloc{};
        });
  }

  /**
   * @brief Acquires a mutable runtime owner for a close-racing test.
   *
   * @param kernel Kernel whose private graph registry is inspected.
   * @param name Graph name to resolve.
   * @return Shared runtime owner retaining the exact published instance.
   * @throws std::runtime_error when the graph is not loaded.
   * @throws std::system_error if graph-registry locking fails.
   * @note Tests that overlap runtime access with close must retain this owner
   *       for their full access interval. It preserves object lifetime but
   *       does not bypass stopped lane admission.
   */
  static std::shared_ptr<GraphRuntime> runtime_owner(Kernel& kernel,
                                                     const std::string& name) {
    auto runtime = kernel.acquire_runtime(name);
    if (!runtime) {
      throw std::runtime_error("Graph not found: " + name);
    }
    return runtime;
  }

  /**
   * @brief Acquires a const runtime owner for a close-racing test.
   *
   * @param kernel Kernel whose private graph registry is inspected.
   * @param name Graph name to resolve.
   * @return Shared const runtime owner retaining the exact published instance.
   * @throws std::runtime_error when the graph is not loaded.
   * @throws std::system_error if graph-registry locking fails.
   * @note Prefer copied Host values unless the assertion needs an internal
   *       runtime collaborator.
   */
  static std::shared_ptr<const GraphRuntime> runtime_owner(
      const Kernel& kernel, const std::string& name) {
    auto runtime = kernel.acquire_runtime(name);
    if (!runtime) {
      throw std::runtime_error("Graph not found: " + name);
    }
    return runtime;
  }

  /**
   * @brief Resolves a mutable runtime owned by a Kernel test fixture.
   *
   * @param kernel Kernel whose private graph map should be inspected.
   * @param name Graph name to resolve.
   * @return Mutable runtime for the named graph.
   * @throws std::runtime_error when the graph is not loaded.
   * @throws std::system_error if graph-registry locking fails.
   * @note This borrowed-reference convenience requires the caller to exclude
   * concurrent close. Use runtime_owner() when close can overlap. Prefer copied
   * public `ps::Host` inspection values when a test validates
   * frontend-visible behavior and does not need runtime-only collaborators.
   */
  static GraphRuntime& runtime(Kernel& kernel, const std::string& name) {
    return *runtime_owner(kernel, name);
  }

  /**
   * @brief Resolves a const runtime owned by a Kernel test fixture.
   *
   * @param kernel Kernel whose private graph map should be inspected.
   * @param name Graph name to resolve.
   * @return Const runtime for the named graph.
   * @throws std::runtime_error when the graph is not loaded.
   * @throws std::system_error if graph-registry locking fails.
   * @note This borrowed-reference helper requires the caller to exclude
   * concurrent close. It is intended for read-only assertions that cannot yet
   * be expressed through a stable value inspection API.
   */
  static const GraphRuntime& runtime(const Kernel& kernel,
                                     const std::string& name) {
    return *runtime_owner(kernel, name);
  }

  /**
   * @brief Resolves the mutable model for a loaded graph.
   *
   * @param kernel Kernel whose graph model should be inspected.
   * @param name Graph name to resolve.
   * @return Mutable GraphModel owned by the named runtime.
   * @throws std::runtime_error when the graph is not loaded.
   * @throws std::system_error if graph-registry locking fails.
   * @note Callers are responsible for excluding concurrent close and mutation
   * unless they use submit_graph_state(), which serializes through
   * GraphStateExecutor and retains accepted work through close drainage.
   */
  static GraphModel& model(Kernel& kernel, const std::string& name) {
    return runtime(kernel, name).model();
  }

  /**
   * @brief Submits a serialized GraphModel operation for a loaded graph.
   *
   * @tparam Fn Callable invocable as fn(GraphModel&).
   * @param kernel Kernel whose graph-state executor should run the operation.
   * @param name Graph name to resolve.
   * @param fn Callable forwarded to GraphStateExecutor::submit().
   * @return Future for the callable result.
   * @throws std::runtime_error when the graph is not loaded.
   * @throws std::system_error if graph-registry or graph-state synchronization
   * fails.
   * @note This replaces the removed Kernel::post() test escape hatch for tests
   * that must assert graph-state serialization behavior directly. It is not a
   * production or public Host operation.
   */
  template <typename Fn>
  static auto submit_graph_state(Kernel& kernel, const std::string& name,
                                 Fn&& fn)
      -> std::future<std::invoke_result_t<Fn, GraphModel&>> {
    auto runtime = runtime_owner(kernel, name);
    return runtime->graph_state().submit(std::forward<Fn>(fn));
  }

  /**
   * @brief Admits one cache-save work item that borrows Kernel collaborators.
   *
   * @param kernel Kernel whose cache service and runtime lane are exercised.
   * @param name Loaded graph name used to resolve the runtime.
   * @param node_id Node whose formal HP output should be persisted.
   * @param precision Cache precision label forwarded to GraphCacheService.
   * @return Future completing after the admitted cache-save work item returns.
   * @throws std::runtime_error when the graph is not loaded.
   * @throws std::bad_alloc if task, precision, or future-state allocation
   * fails.
   * @throws std::system_error if graph-registry or graph-state synchronization
   * fails.
   * @note The submitted closure intentionally borrows `kernel.cache_service_`
   * through the real graph-state lane. It is a private lifetime-regression
   * seam, not a production cache API or an alternate ownership wrapper.
   */
  static std::future<void> submit_cache_save(Kernel& kernel,
                                             const std::string& name,
                                             int node_id,
                                             std::string precision) {
    auto runtime = runtime_owner(kernel, name);
    return runtime->graph_state().submit(
        [&kernel, node_id,
         precision = std::move(precision)](GraphModel& graph) {
          kernel.cache_service_.save_cache_if_configured(
              graph, graph.node(node_id), precision);
        });
  }

  /**
   * @brief Clears the runtime execution trace for a loaded graph.
   *
   * @param kernel Kernel whose runtime trace should be reset.
   * @param name Graph name to resolve.
   * @throws std::runtime_error when the graph is not loaded.
   * @throws std::system_error if graph-registry or trace synchronization fails.
   * @note Production callers can read copied execution trace values through
   * public `ps::Host`; clearing trace remains a test-only setup operation.
   */
  static void clear_execution_trace(Kernel& kernel, const std::string& name) {
    runtime_owner(kernel, name)->clear_execution_trace();
  }

  /**
   * @brief Copies the runtime execution trace for a loaded graph.
   *
   * @param kernel Kernel whose runtime trace should be read.
   * @param name Graph name to resolve.
   * @param after_sequence Exclusive sequence cursor.
   * @param limit Maximum events to copy.
   * @return One bounded non-destructive internal trace page.
   * @throws std::runtime_error when the graph is not loaded.
   * @throws std::invalid_argument for invalid bounds or a future cursor.
   * @throws std::bad_alloc if bounded page allocation fails.
   * @throws std::system_error if graph-registry or trace synchronization fails.
   * @note Prefer `ps::Host::execution_trace()` for frontend-visible value-level
   * assertions. This helper exposes no unbounded production getter.
   */
  static GraphRuntime::ExecutionEventPage execution_trace(
      Kernel& kernel, const std::string& name, uint64_t after_sequence = 0,
      std::size_t limit = kExecutionTraceMaxLimit) {
    return runtime_owner(kernel, name)
        ->execution_trace_page(after_sequence, limit);
  }
};

}  // namespace ps::testing
