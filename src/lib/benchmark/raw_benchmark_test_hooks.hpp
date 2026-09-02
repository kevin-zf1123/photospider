#pragma once

#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/execution.hpp"

namespace ps::benchmark_testing {

/**
 * @brief Callback that replaces one completed compilation diagnostic record.
 * @param diagnostics Mutable successful compiler output owned by the runner.
 * @return No value.
 * @throws Any exception deliberately raised by the installed test callback.
 * @note The callback runs before the runner copies the record into a sample.
 */
using CompilationDiagnosticsHook = void (*)(CompilationDiagnostics&);

/**
 * @brief Callback that observes or replaces a completed execution record.
 * @param diagnostics Mutable successful executor output owned by the runner.
 * @return No value.
 * @throws Any exception deliberately raised by the installed test callback.
 * @note The callback runs before the runner copies the record and observes
 * cancellation before optional oracle invocation. Tests may therefore use it
 * to expose the otherwise private post-execute publication window.
 */
using ExecutionDiagnosticsHook = void (*)(ExecutionDiagnostics&);

/**
 * @brief Private deterministic callbacks for raw-benchmark regression tests.
 *
 * @note This structure is private to the noninstalled test-kernel variant and
 * has no effect unless a test explicitly installs one process-global
 * immutable callback set.
 */
struct RawBenchmarkTestHooks final {
  /** @brief Optional completed-compilation diagnostic replacement. */
  CompilationDiagnosticsHook completed_compilation = nullptr;
  /** @brief Optional completed-execution observation or replacement. */
  ExecutionDiagnosticsHook completed_execution = nullptr;
};

/**
 * @brief Installs or clears the private process-global benchmark callbacks.
 * @param hooks Borrowed callback set, or null to clear.
 * @return No value.
 * @throws Nothing.
 * @note The caller keeps a nonnull set alive and immutable until clearing it;
 * tests must not install competing sets concurrently.
 */
void install_raw_benchmark_test_hooks(
    const RawBenchmarkTestHooks* hooks) noexcept;

/**
 * @brief Applies the installed completed-compilation diagnostic callback.
 * @param diagnostics Mutable successful compiler output owned by the runner.
 * @return No value.
 * @throws Any exception deliberately raised by the installed test callback.
 * @note Only the noninstalled test-kernel variant calls this function.
 */
void apply_completed_compilation_hook(CompilationDiagnostics& diagnostics);

/**
 * @brief Applies the installed completed-execution observation callback.
 * @param diagnostics Mutable successful executor output owned by the runner.
 * @return No value.
 * @throws Any exception deliberately raised by the installed test callback.
 * @note Only the noninstalled test-kernel variant calls this function.
 */
void apply_completed_execution_hook(ExecutionDiagnostics& diagnostics);

}  // namespace ps::benchmark_testing
