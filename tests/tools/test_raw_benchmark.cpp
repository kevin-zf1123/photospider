#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/raw_benchmark_test_hooks.hpp"
#include "photospider/benchmark/raw_benchmark.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using FixturePoll = ps::Result<ps::ResultProgramPoll>;
// NOLINTBEGIN(whitespace/indent_namespace)
using FixtureCallback =
    std::function<FixturePoll(const ps::ResultProgramPhase&)>;
// NOLINTEND
FixturePoll publish_number(const ps::ResultProgramPhase& phase, double number) {
  auto builder = multi_result::take(ps::ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {}, {}, phase.query.tile_extent));
  multi_result::check(builder.publish_tensor(
      0, ps::Region::whole({1}),
      ps::ByteView(reinterpret_cast<const std::uint8_t*>(&number),
                   sizeof(number))));
  return FixturePoll(
      ps::ResultPublication{multi_result::take(builder.seal()), true});
}
struct FixtureProgram {
  FixtureCallback compute;
  explicit FixtureProgram(FixtureCallback callback)
      : compute(std::move(callback)) {}
  FixturePoll poll(const ps::ResultProgramPhase& phase) {
    return compute(phase);
  }
};
ps::OperationDefinition fixture_operation(std::string key,
                                          ps::OperationTraits traits,
                                          FixtureCallback compute) {
  traits.outputs = {multi_result::output("value")};
  traits.outputs[0].continuation_bytes = sizeof(FixtureProgram);
  traits.cacheable = false;
  ps::OperationDefinition definition;
  definition.key = std::move(key);
  definition.traits = std::move(traits);
  definition.start_result = [compute = std::move(compute)](
                                const ps::ResultProgramQuery&,
                                const ps::BufferAllocator& allocator) {
    return ps::ResultContinuation::make<FixtureProgram>(allocator, compute);
  };
  return definition;
}
ps::WorkflowDocument single_operation_document(std::string key) {
  ps::WorkflowDocument document;
  document.nodes = {{1, std::move(key), {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  return document;
}
/**
 * @brief Standard exception whose borrowed diagnostic pointer is null.
 *
 * @note Benchmark oracle regressions use this allocation-free exception to
 * prove that an absent diagnostic cannot escape or outrank cancellation.
 */
class NullDiagnosticException final : public std::exception {
 public:
  /**
   * @brief Returns no diagnostic bytes for the injected oracle failure.
   * @return Null, intentionally.
   * @throws Nothing.
   */
  [[nodiscard]] const char* what() const noexcept override { return nullptr; }
};

/**
 * @brief Deterministically holds a successful raw-benchmark oracle.
 *
 * @note The oracle owns no cancellation source. A separate test thread wins
 * cancellation while the callback is held, matching the non-preemptible
 * caller-oracle boundary.
 */
class BenchmarkOracleGate final {
 public:
  /**
   * @brief Blocks the oracle until the test explicitly releases it.
   * @return No value.
   * @throws std::system_error If test synchronization fails.
   * @note The gate signals entry before waiting so cancellation can occur
   * while the oracle is executing.
   */
  void hold() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
  }

  /**
   * @brief Waits for the oracle to enter the held callback boundary.
   * @param timeout Maximum bounded wait.
   * @return True when entry was observed before the deadline.
   * @throws std::system_error If test synchronization fails.
   */
  bool wait_until_entered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return entered_; });
  }

  /**
   * @brief Releases the held oracle callback.
   * @return No value.
   * @throws std::system_error If mutex locking fails.
   */
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

 private:
  /** @brief Serializes gate state. */
  std::mutex mutex_;
  /** @brief Wakes the test and held oracle after state changes. */
  std::condition_variable changed_;
  /** @brief Whether the oracle entered the held boundary. */
  bool entered_ = false;
  /** @brief Whether the held oracle may return. */
  bool released_ = false;
};

/** @brief Number of completed-compilation hook invocations in this test. */
std::uint32_t g_compilation_diagnostics_hook_calls = 0U;

/** @brief Number of completed-execution hook invocations in this test. */
std::uint32_t g_execution_diagnostics_hook_calls = 0U;

/**
 * @brief Borrowed cancellation source used by one completed-execution hook.
 *
 * @note The source remains alive until the scoped test clears the installed
 * hook and this pointer; no competing benchmark test runs concurrently.
 */
ps::CancellationSource* g_completed_execution_cancellation = nullptr;

/**
 * @brief Returns legal deterministic compiler timing sentinels.
 * @return Immutable diagnostics with one legitimate zero-microsecond stage.
 * @throws Nothing.
 * @note Zero is valid because a stage may complete within clock resolution.
 */
const ps::CompilationDiagnostics& sentinel_compilation_diagnostics() noexcept {
  static const ps::CompilationDiagnostics diagnostics{0U, 11U, 13U};
  return diagnostics;
}

/**
 * @brief Builds internally coherent successful execution sentinels.
 * @return Diagnostics containing fallback, transfer, backend, timing, and
 * digest observations distinct from defaults.
 * @throws std::bad_alloc If sentinel container allocation fails.
 * @note The unavailable GPU attempt legitimately has zero duration and the
 * later CPU attempt is the selected backend for that node.
 */
ps::ExecutionDiagnostics make_sentinel_execution_diagnostics() {
  ps::ExecutionDiagnostics diagnostics;
  diagnostics.execute_us = 29U;
  diagnostics.selected_backends = {
      {{1U, 0}, ps::Backend::Cpu},
      {{2U, 0}, ps::Backend::Gpu},
      {{3U, 0}, ps::Backend::Cpu},
  };
  diagnostics.transfer_count = 1U;
  diagnostics.transfer_bytes = sizeof(double);
  diagnostics.peak_live_bytes = 24U;
  diagnostics.fallback_reasons = {"node 1 sentinel GPU fallback"};
  diagnostics.operation_timings = {
      {{1U, 0}, ps::Backend::Gpu, 0U, ps::ErrorCode::BackendUnavailable},
      {{1U, 0}, ps::Backend::Cpu, 3U, ps::ErrorCode::Ok},
      {{2U, 0}, ps::Backend::Gpu, 5U, ps::ErrorCode::Ok},
      {{3U, 0}, ps::Backend::Cpu, 7U, ps::ErrorCode::Ok},
  };
  diagnostics.plan_digest = std::string(64U, 'a');
  diagnostics.result_digest = std::string(64U, 'b');
  return diagnostics;
}

/**
 * @brief Returns legal deterministic execution diagnostic sentinels.
 * @return Immutable diagnostics shared by both oracle-failure paths.
 * @throws std::bad_alloc On first-call sentinel construction failure.
 * @note The returned object remains alive until process shutdown.
 */
const ps::ExecutionDiagnostics& sentinel_execution_diagnostics() {
  static const ps::ExecutionDiagnostics diagnostics =
      make_sentinel_execution_diagnostics();
  return diagnostics;
}

/**
 * @brief Replaces completed compiler diagnostics with deterministic sentinels.
 * @param diagnostics Mutable successful compiler output.
 * @return No value.
 * @throws Nothing.
 * @note The call count proves that the test seam executed before sample copy.
 */
void inject_compilation_diagnostics(
    ps::CompilationDiagnostics& diagnostics) noexcept {
  ++g_compilation_diagnostics_hook_calls;
  diagnostics = sentinel_compilation_diagnostics();
}

/**
 * @brief Replaces completed executor diagnostics with deterministic sentinels.
 * @param diagnostics Mutable successful executor output.
 * @return No value.
 * @throws std::bad_alloc If copying sentinel containers fails.
 * @note The call count proves that the test seam executed before sample copy.
 */
void inject_execution_diagnostics(ps::ExecutionDiagnostics& diagnostics) {
  ++g_execution_diagnostics_hook_calls;
  diagnostics = sentinel_execution_diagnostics();
}

/**
 * @brief Cancels one benchmark after execution but before sample publication.
 * @param diagnostics Completed diagnostics left unchanged.
 * @return No value.
 * @throws Nothing.
 * @note This hook exists only in the noninstalled benchmark test library and
 * creates the otherwise unobservable no-oracle post-execute window.
 */
void cancel_after_completed_execution(
    ps::ExecutionDiagnostics& diagnostics) noexcept {
  static_cast<void>(diagnostics);
  if (g_completed_execution_cancellation) {
    static_cast<void>(g_completed_execution_cancellation->cancel());
  }
}

}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto operations = std::make_shared<OperationRegistry>();
  PS_CHECK(operations
               ->register_operation(fixture_operation(
                   "test.benchmark_constant", OperationTraits{},
                   [](const ResultProgramPhase& phase) {
                     return publish_number(phase, 6.5);
                   }))
               .ok());
  operations->freeze();
  Compiler compiler(operations);
  ExecutionContext execution(operations);
  auto document = single_operation_document("test.benchmark_constant");
  document.outputs[0].name = "sum";
  GraphContext graph(std::move(document));
  RawBenchmarkRunner benchmark(&compiler, &execution);
  auto benchmark_cancellation_operations =
      std::make_shared<OperationRegistry>();
  CancellationSource* active_benchmark_cancellation = nullptr;
  std::uint32_t benchmark_callback_count = 0U;
  std::uint32_t cancel_on_callback = 0U;
  PS_CHECK(benchmark_cancellation_operations
               ->register_operation(fixture_operation(
                   "test.benchmark_cancel", OperationTraits{},
                   [&active_benchmark_cancellation, &benchmark_callback_count,
                    &cancel_on_callback](
                       const ResultProgramPhase& invocation) -> FixturePoll {
                     ++benchmark_callback_count;
                     if (active_benchmark_cancellation != nullptr &&
                         benchmark_callback_count == cancel_on_callback) {
                       static_cast<void>(
                           active_benchmark_cancellation->cancel());
                     }
                     return publish_number(invocation, 1.0);
                   }))
               .ok());
  benchmark_cancellation_operations->freeze();
  Compiler benchmark_cancellation_compiler(benchmark_cancellation_operations);
  ExecutionContext benchmark_cancellation_execution(
      benchmark_cancellation_operations,
      ExecutionContextConfig{1U, false, 4U, 65536U});
  GraphContext benchmark_cancellation_graph(
      single_operation_document("test.benchmark_cancel"));
  RawBenchmarkRunner cancellation_benchmark(&benchmark_cancellation_compiler,
                                            &benchmark_cancellation_execution);

  {
    CancellationSource callback_cancellation;
    active_benchmark_cancellation = &callback_cancellation;
    benchmark_callback_count = 0U;
    cancel_on_callback = 1;
    RawBenchmarkOptions cancellation_options;
    cancellation_options.iterations = 1;
    auto cancelled_report = cancellation_benchmark.run(
        benchmark_cancellation_graph, cancellation_options,
        callback_cancellation.token());
    PS_CHECK(!cancelled_report.ok());
    PS_CHECK(cancelled_report.status().code == ErrorCode::Cancelled);
    PS_CHECK(benchmark_callback_count == 1);
  }
  active_benchmark_cancellation = nullptr;

  BenchmarkOracleGate benchmark_oracle_gate;
  CancellationSource barrier_cancellation;
  RawBenchmarkOptions barrier_oracle_options;
  barrier_oracle_options.correctness_oracle =
      [&benchmark_oracle_gate](
          const ExecutionResult&) -> CorrectnessObservation {
    benchmark_oracle_gate.hold();
    return CorrectnessObservation{true, {}};
  };
  barrier_oracle_options.oracle_name = "barrier-accepts";
  auto barrier_oracle_future = std::async(std::launch::async, [&] {
    return benchmark.run(graph, barrier_oracle_options,
                         barrier_cancellation.token());
  });
  const bool oracle_was_running =
      benchmark_oracle_gate.wait_until_entered(std::chrono::seconds(2));
  const bool barrier_cancel_won = barrier_cancellation.cancel();
  benchmark_oracle_gate.release();
  auto barrier_oracle_report = barrier_oracle_future.get();
  PS_CHECK(oracle_was_running);
  PS_CHECK(barrier_cancel_won);
  PS_CHECK(!barrier_oracle_report.ok());
  PS_CHECK(barrier_oracle_report.status().code == ErrorCode::Cancelled);

  CancellationSource post_execute_cancellation;
  g_completed_execution_cancellation = &post_execute_cancellation;
  const ps::benchmark_testing::RawBenchmarkTestHooks cancellation_test_hooks{
      nullptr,
      &cancel_after_completed_execution,
  };
  ps::benchmark_testing::install_raw_benchmark_test_hooks(
      &cancellation_test_hooks);
  RawBenchmarkOptions post_execute_options;
  auto post_execute_report = benchmark.run(graph, post_execute_options,
                                           post_execute_cancellation.token());
  ps::benchmark_testing::install_raw_benchmark_test_hooks(nullptr);
  g_completed_execution_cancellation = nullptr;
  PS_CHECK(!post_execute_report.ok());
  PS_CHECK(post_execute_report.status().code == ErrorCode::Cancelled);

  const auto& expected_compilation = sentinel_compilation_diagnostics();
  const auto& expected_execution = sentinel_execution_diagnostics();
  const ps::benchmark_testing::RawBenchmarkTestHooks benchmark_test_hooks{
      &inject_compilation_diagnostics, &inject_execution_diagnostics};
  ps::benchmark_testing::install_raw_benchmark_test_hooks(
      &benchmark_test_hooks);
  std::uint32_t null_oracle_calls = 0U;
  RawBenchmarkOptions null_oracle_options;
  null_oracle_options.iterations = 2U;
  null_oracle_options.correctness_oracle =
      [&null_oracle_calls](const ExecutionResult&) -> CorrectnessObservation {
    ++null_oracle_calls;
    if (null_oracle_calls == 1U) {
      throw NullDiagnosticException();
    }
    return CorrectnessObservation{true, "accepted after null diagnostic"};
  };
  null_oracle_options.oracle_name = "null-then-accepts";
  bool null_oracle_escaped = false;
  std::optional<ps::Result<ps::RawBenchmarkReport>> null_oracle_report;
  try {
    null_oracle_report.emplace(benchmark.run(graph, null_oracle_options));
  } catch (...) {
    null_oracle_escaped = true;
  }
  PS_CHECK(!null_oracle_escaped);
  PS_CHECK(null_oracle_report.has_value());
  PS_CHECK(null_oracle_report->ok());
  PS_CHECK(null_oracle_calls == 2U);
  PS_CHECK(g_compilation_diagnostics_hook_calls == 2U);
  PS_CHECK(g_execution_diagnostics_hook_calls == 2U);
  PS_CHECK(null_oracle_report->value().oracle_name == "null-then-accepts");
  PS_CHECK(null_oracle_report->value().samples.size() == 2U);
  const RawBenchmarkSample& null_diagnostic_sample =
      null_oracle_report->value().samples[0U];
  const RawBenchmarkSample& recovered_oracle_sample =
      null_oracle_report->value().samples[1U];
  PS_CHECK(null_diagnostic_sample.iteration == 0U);
  PS_CHECK(null_diagnostic_sample.oracle_name == "null-then-accepts");
  PS_CHECK(null_diagnostic_sample.correctness_checked);
  PS_CHECK(!null_diagnostic_sample.correctness_accepted);
  PS_CHECK(null_diagnostic_sample.outcome == ErrorCode::OperationFailed);
  PS_CHECK(null_diagnostic_sample.reason.empty());
  PS_CHECK(recovered_oracle_sample.iteration == 1U);
  PS_CHECK(recovered_oracle_sample.oracle_name == "null-then-accepts");
  PS_CHECK(recovered_oracle_sample.correctness_checked);
  PS_CHECK(recovered_oracle_sample.correctness_accepted);
  PS_CHECK(recovered_oracle_sample.outcome == ErrorCode::Ok);
  PS_CHECK(recovered_oracle_sample.reason == "accepted after null diagnostic");
  for (const RawBenchmarkSample* sample :
       {&null_diagnostic_sample, &recovered_oracle_sample}) {
    PS_CHECK(sample->compilation.analyze_us == expected_compilation.analyze_us);
    PS_CHECK(sample->compilation.optimize_us ==
             expected_compilation.optimize_us);
    PS_CHECK(sample->compilation.plan_us == expected_compilation.plan_us);
    PS_CHECK(sample->execution.execute_us == expected_execution.execute_us);
    PS_CHECK(sample->execution.selected_backends ==
             expected_execution.selected_backends);
    PS_CHECK(sample->execution.transfer_count ==
             expected_execution.transfer_count);
    PS_CHECK(sample->execution.transfer_bytes ==
             expected_execution.transfer_bytes);
    PS_CHECK(sample->execution.peak_live_bytes ==
             expected_execution.peak_live_bytes);
    PS_CHECK(sample->execution.fallback_reasons ==
             expected_execution.fallback_reasons);
    PS_CHECK(sample->execution.plan_digest == expected_execution.plan_digest);
    PS_CHECK(sample->execution.result_digest ==
             expected_execution.result_digest);
    PS_CHECK(sample->execution.operation_timings.size() ==
             expected_execution.operation_timings.size());
    for (std::size_t index = 0U;
         index < sample->execution.operation_timings.size(); ++index) {
      const auto& actual = sample->execution.operation_timings[index];
      const auto& expected = expected_execution.operation_timings[index];
      PS_CHECK(actual.output == expected.output);
      PS_CHECK(actual.backend == expected.backend);
      PS_CHECK(actual.duration_us == expected.duration_us);
      PS_CHECK(actual.outcome == expected.outcome);
    }
  }

  CancellationSource null_oracle_cancellation;
  RawBenchmarkOptions cancelled_null_oracle_options;
  cancelled_null_oracle_options.correctness_oracle =
      [&null_oracle_cancellation](
          const ExecutionResult&) -> CorrectnessObservation {
    static_cast<void>(null_oracle_cancellation.cancel());
    throw NullDiagnosticException();
  };
  cancelled_null_oracle_options.oracle_name = "cancelled-null-oracle";
  bool cancelled_null_oracle_escaped = false;
  std::optional<ps::Result<ps::RawBenchmarkReport>>
      cancelled_null_oracle_report;
  try {
    cancelled_null_oracle_report.emplace(
        benchmark.run(graph, cancelled_null_oracle_options,
                      null_oracle_cancellation.token()));
  } catch (...) {
    cancelled_null_oracle_escaped = true;
  }
  PS_CHECK(!cancelled_null_oracle_escaped);
  PS_CHECK(cancelled_null_oracle_report.has_value());
  PS_CHECK(!cancelled_null_oracle_report->ok());
  PS_CHECK(cancelled_null_oracle_report->status().code == ErrorCode::Cancelled);
  PS_CHECK(g_compilation_diagnostics_hook_calls == 3U);
  PS_CHECK(g_execution_diagnostics_hook_calls == 3U);
  ps::benchmark_testing::install_raw_benchmark_test_hooks(nullptr);

  return 0;
}
