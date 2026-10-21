#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
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
ps::OperationDefinition fixture_operation(std::string key,
                                          std::function<double()> compute) {
  ps::OperationDefinition definition;
  definition.key = std::move(key);
  definition.traits.outputs = {multi_result::output("value")};
  multi_result::install_fixed(definition, {-1}, {0});
  definition.prepare_kernel =
      [compute = std::move(compute)](
          const auto&, const auto&, const auto& query, const auto&,
          const auto*) -> ps::Result<ps_prepared_kernel_v8> {
    if (!query.reads.empty() || query.writes.size() != 1 ||
        query.writes[0].bytes != sizeof(double))
      return ps::Result<ps_prepared_kernel_v8>(
          {ps::ErrorCode::TypeMismatch, "benchmark fixture signature"});
    ps_prepared_kernel_v8 kernel{};
    kernel.struct_size = sizeof(kernel);
    kernel.abi_version = 8;
    kernel.context = new std::function<double()>(compute);
    kernel.release = [](const void* context) {
      delete static_cast<const std::function<double()>*>(context);
    };
    kernel.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      const auto& callback = *static_cast<const std::function<double()>*>(
          invocation->prepared_context);
      const double value = callback();
      const auto& output = invocation->outputs[0];
      std::memcpy(
          static_cast<std::uint8_t*>(output.allocation) + output.byte_offset,
          &value, sizeof(value));
      *status = {};
    };
    return ps::Result<ps_prepared_kernel_v8>(kernel);
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
 * @return Fixed execution counters distinct from their initial values.
 */
ps::ExecutionDiagnostics make_sentinel_execution_diagnostics() {
  ps::ExecutionDiagnostics diagnostics;
  diagnostics.activations = 1;
  diagnostics.submitted_tasks = 7;
  diagnostics.completed_tasks = 7;
  diagnostics.retired_calls = 3;
  diagnostics.peak_active_tasks = 2;
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
               ->register_operation(fixture_operation("test.benchmark_constant",
                                                      [] { return 6.5; }))
               .ok());
  unsigned preparations = 0;
  auto retry = fixture_operation("test.benchmark_prepare", [] { return 3.0; });
  const auto prepare = retry.prepare_kernel;
  retry.prepare_kernel =
      [&, prepare](const auto& kernel, const auto& placement, const auto& query,
                   const auto& resources,
                   const auto* device) -> Result<ps_prepared_kernel_v8> {
    if (++preparations == 1)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::OperationFailed, "first preparation rejected"});
    return prepare(kernel, placement, query, resources, device);
  };
  PS_REQUIRE_OK(operations->register_operation(std::move(retry)));
  unsigned executions = 0;
  PS_REQUIRE_OK(operations->register_operation(
      fixture_operation("test.benchmark_execution", [&] {
        if (++executions == 1)
          throw std::runtime_error("first invocation rejected");
        return 3.0;
      })));
  operations->freeze();
  Compiler compiler(operations);
  ExecutionContext execution(operations);
  auto document = single_operation_document("test.benchmark_constant");
  document.outputs[0].name = "sum";
  GraphContext graph(std::move(document));
  RawBenchmarkRunner benchmark(&compiler, &execution);
  RawBenchmarkOptions successful_options;
  successful_options.iterations = 2;
  successful_options.oracle_name = "constant-value";
  successful_options.correctness_oracle = [](const ExecutionResult& result) {
    return CorrectnessObservation{
        multi_result::number(result.results.at("sum")) == 6.5,
        {}};
  };
  successful_options.execution.trace_capacity = 1;
  auto successful = benchmark.run(graph, successful_options);
  PS_REQUIRE_OK(successful);
  PS_CHECK(successful.value().samples.size() == 2);
  PS_CHECK(successful.value().cpu_workers == execution.cpu_workers());
  PS_CHECK(successful.value().native_device_description.empty());
  for (const auto& sample : successful.value().samples)
    PS_CHECK(sample.outcome == ErrorCode::Ok && sample.correctness_checked &&
             sample.correctness_accepted && sample.execution.activations == 1 &&
             sample.execution.completed_tasks == 1 &&
             sample.execution.submitted_tasks == 1 &&
             sample.execution.retired_calls == 1 &&
             sample.task_timings.size() == 1 &&
             sample.call_timings.size() == 1 &&
             sample.task_timings[0].status == ErrorCode::Ok &&
             sample.task_timings[0].completion_ns <=
                 sample.task_timings[0].consumed_ns &&
             sample.call_timings[0].outcome == CallOutcome::Succeeded);
  RawBenchmarkOptions failure_options;
  failure_options.iterations = 2;
  failure_options.execution.trace_capacity = 1;
  GraphContext prepare_graph(
      single_operation_document("test.benchmark_prepare"));
  auto failed_preparation = benchmark.run(prepare_graph, failure_options);
  PS_REQUIRE_OK(failed_preparation);
  PS_CHECK(preparations == 2 && failed_preparation.value().samples.size() == 2);
  PS_CHECK(failed_preparation.value().samples[0].outcome ==
               ErrorCode::OperationFailed &&
           failed_preparation.value().samples[0].reason ==
               "first preparation rejected" &&
           failed_preparation.value().samples[0].execution.submitted_tasks ==
               0);
  PS_CHECK(failed_preparation.value().samples[1].outcome == ErrorCode::Ok &&
           failed_preparation.value().samples[1].execution.completed_tasks ==
               1);
  GraphContext execution_graph(
      single_operation_document("test.benchmark_execution"));
  auto failed_execution = benchmark.run(execution_graph, failure_options);
  PS_REQUIRE_OK(failed_execution);
  PS_CHECK(executions == 2 && failed_execution.value().samples.size() == 2);
  const auto& failed_sample = failed_execution.value().samples[0];
  PS_CHECK(failed_sample.outcome == ErrorCode::OperationFailed &&
           failed_sample.reason == "first invocation rejected" &&
           failed_sample.execution.submitted_tasks == 1 &&
           failed_sample.execution.completed_tasks == 1 &&
           failed_sample.execution.allocation_count == 1 &&
           failed_sample.prepare_ns > 0 && failed_sample.submit_ns > 0 &&
           failed_sample.join_ns > 0 &&
           failed_sample.task_timings.size() == 1 &&
           failed_sample.task_timings[0].status == ErrorCode::OperationFailed);
  PS_CHECK(failed_sample.call_timings.size() == 1);
  PS_CHECK(failed_sample.call_timings[0].outcome == CallOutcome::Failed);
  PS_CHECK(failed_execution.value().samples[1].outcome == ErrorCode::Ok);
  failure_options.bindings.inputs = {{"unexpected", {}}};
  auto failed_submission = benchmark.run(graph, failure_options);
  PS_REQUIRE_OK(failed_submission);
  PS_CHECK(failed_submission.value().samples.size() == 2);
  for (const auto& sample : failed_submission.value().samples)
    PS_CHECK(sample.outcome == ErrorCode::InvalidArgument &&
             sample.execution.submitted_tasks == 0 &&
             !sample.correctness_checked);
  auto benchmark_cancellation_operations =
      std::make_shared<OperationRegistry>();
  CancellationSource* active_benchmark_cancellation = nullptr;
  std::uint32_t benchmark_callback_count = 0U;
  std::uint32_t cancel_on_callback = 0U;
  PS_CHECK(benchmark_cancellation_operations
               ->register_operation(fixture_operation(
                   "test.benchmark_cancel",
                   [&active_benchmark_cancellation, &benchmark_callback_count,
                    &cancel_on_callback]() {
                     ++benchmark_callback_count;
                     if (active_benchmark_cancellation != nullptr &&
                         benchmark_callback_count == cancel_on_callback) {
                       static_cast<void>(
                           active_benchmark_cancellation->cancel());
                     }
                     return 1.0;
                   }))
               .ok());
  benchmark_cancellation_operations->freeze();
  Compiler benchmark_cancellation_compiler(benchmark_cancellation_operations);
  ExecutionContextConfig cancellation_config;
  cancellation_config.cpu_workers = 1;
  cancellation_config.maximum_queued_tasks = 4;
  cancellation_config.maximum_live_bytes = 65536;
  ExecutionContext benchmark_cancellation_execution(
      benchmark_cancellation_operations, cancellation_config);
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
    PS_CHECK(sample->execution.activations == expected_execution.activations);
    PS_CHECK(sample->execution.submitted_tasks ==
             expected_execution.submitted_tasks);
    PS_CHECK(sample->execution.completed_tasks ==
             expected_execution.completed_tasks);
    PS_CHECK(sample->execution.retired_calls ==
             expected_execution.retired_calls);
    PS_CHECK(sample->execution.peak_active_tasks ==
             expected_execution.peak_active_tasks);
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
