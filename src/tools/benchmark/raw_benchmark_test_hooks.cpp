#include "benchmark/raw_benchmark_test_hooks.hpp"

#include <atomic>

namespace ps::benchmark_testing {
namespace {

/** @brief Borrowed active callback set, or null outside one scoped test. */
std::atomic<const RawBenchmarkTestHooks*> g_hooks{nullptr};

}  // namespace

/**
 * @brief Implements private raw-benchmark callback installation.
 * @copydetails install_raw_benchmark_test_hooks
 */
void install_raw_benchmark_test_hooks(
    const RawBenchmarkTestHooks* hooks) noexcept {
  g_hooks.store(hooks, std::memory_order_release);
}

/**
 * @brief Implements completed-compilation diagnostic replacement.
 * @copydetails apply_completed_compilation_hook
 */
void apply_completed_compilation_hook(CompilationDiagnostics& diagnostics) {
  const RawBenchmarkTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->completed_compilation) {
    hooks->completed_compilation(diagnostics);
  }
}

/**
 * @brief Implements completed-execution diagnostic replacement.
 * @copydetails apply_completed_execution_hook
 */
void apply_completed_execution_hook(ExecutionDiagnostics& diagnostics) {
  const RawBenchmarkTestHooks* hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->completed_execution) {
    hooks->completed_execution(diagnostics);
  }
}

}  // namespace ps::benchmark_testing
