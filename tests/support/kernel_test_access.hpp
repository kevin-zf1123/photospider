#pragma once

#include <future>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "kernel/kernel.hpp"

namespace ps::testing {

/**
 * @brief Internal-only access bridge for tests that must inspect Kernel state.
 *
 * KernelTestAccess centralizes test-only runtime and graph-state access after
 * Kernel removed its public runtime/post escape hatches. Production callers
 * must use Kernel and InteractionService value facades instead; this header is
 * only included by test targets through the tests/support include path.
 *
 * @note Methods throw std::runtime_error for missing graphs so tests fail
 * loudly instead of silently treating a missing internal runtime as success.
 */
class KernelTestAccess {
 public:
  /**
   * @brief Resolves a mutable runtime owned by a Kernel test fixture.
   *
   * @param kernel Kernel whose private graph map should be inspected.
   * @param name Graph name to resolve.
   * @return Mutable runtime for the named graph.
   * @throws std::runtime_error when the graph is not loaded.
   * @note This is a test-only boundary. Prefer public Kernel inspection values
   * when a test does not need runtime-only collaborators.
   */
  static GraphRuntime& runtime(Kernel& kernel, const std::string& name) {
    auto it = kernel.graphs_.find(name);
    if (it == kernel.graphs_.end()) {
      throw std::runtime_error("Graph not found: " + name);
    }
    return *it->second;
  }

  /**
   * @brief Resolves a const runtime owned by a Kernel test fixture.
   *
   * @param kernel Kernel whose private graph map should be inspected.
   * @param name Graph name to resolve.
   * @return Const runtime for the named graph.
   * @throws std::runtime_error when the graph is not loaded.
   * @note This helper is intended for read-only assertions that cannot yet be
   * expressed through a stable value inspection API.
   */
  static const GraphRuntime& runtime(const Kernel& kernel,
                                     const std::string& name) {
    auto it = kernel.graphs_.find(name);
    if (it == kernel.graphs_.end()) {
      throw std::runtime_error("Graph not found: " + name);
    }
    return *it->second;
  }

  /**
   * @brief Resolves the mutable model for a loaded graph.
   *
   * @param kernel Kernel whose graph model should be inspected.
   * @param name Graph name to resolve.
   * @return Mutable GraphModel owned by the named runtime.
   * @throws std::runtime_error when the graph is not loaded.
   * @note Callers are responsible for avoiding concurrent mutation unless they
   * use submit_graph_state(), which serializes through GraphStateExecutor.
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
   * @note This replaces the removed public Kernel::post() escape hatch for
   * tests that must assert graph-state serialization behavior directly.
   */
  template <typename Fn>
  static auto submit_graph_state(Kernel& kernel, const std::string& name,
                                 Fn&& fn)
      -> std::future<std::invoke_result_t<Fn, GraphModel&>> {
    return runtime(kernel, name).graph_state().submit(std::forward<Fn>(fn));
  }

  /**
   * @brief Clears the runtime scheduler trace for a loaded graph.
   *
   * @param kernel Kernel whose runtime trace should be reset.
   * @param name Graph name to resolve.
   * @throws std::runtime_error when the graph is not loaded.
   * @note Public callers can read scheduler_trace(); clearing trace remains a
   * test-only setup operation.
   */
  static void clear_scheduler_trace(Kernel& kernel, const std::string& name) {
    runtime(kernel, name).clear_scheduler_log();
  }

  /**
   * @brief Copies the runtime scheduler trace for a loaded graph.
   *
   * @param kernel Kernel whose runtime trace should be read.
   * @param name Graph name to resolve.
   * @return Scheduler events captured by the runtime.
   * @throws std::runtime_error when the graph is not loaded.
   * @note Prefer Kernel::scheduler_trace() for value-level assertions. This
   * helper exists for tests that compare public facade output with internal
   * runtime state.
   */
  static std::vector<GraphRuntime::SchedulerEvent> scheduler_trace(
      Kernel& kernel, const std::string& name) {
    return runtime(kernel, name).get_scheduler_log();
  }
};

}  // namespace ps::testing
