#include "photospider/benchmark/raw_benchmark.hpp"

#include <stdexcept>
#include <utility>

namespace ps {

/**
 * @brief Implements validated raw-benchmark dependency binding.
 * @copydetails RawBenchmarkRunner::RawBenchmarkRunner
 */
RawBenchmarkRunner::RawBenchmarkRunner(Compiler* compiler,
                                       ExecutionContext* execution)
    : compiler_(compiler), execution_(execution) {
  if (!compiler_ || !execution_) {
    throw std::invalid_argument(
        "raw benchmark requires compiler and execution context");
  }
}

/**
 * @brief Implements repeated compile-plan-execute observation.
 * @copydetails RawBenchmarkRunner::run
 */
Result<RawBenchmarkReport> RawBenchmarkRunner::run(
    const GraphContext& graph, const RawBenchmarkOptions& options,
    const CancellationToken& cancellation) const {
  if (options.iterations == 0U) {
    return Result<RawBenchmarkReport>(
        Status::failure(ErrorCode::InvalidArgument,
                        "raw benchmark iteration count must be positive"));
  }
  if (cancellation.cancelled()) {
    return Result<RawBenchmarkReport>(Status::failure(
        ErrorCode::Cancelled, "raw benchmark was cancelled before start"));
  }

  RawBenchmarkReport report;
  report.samples.reserve(options.iterations);
  for (std::uint32_t iteration = 0U; iteration < options.iterations;
       ++iteration) {
    if (cancellation.cancelled()) {
      return Result<RawBenchmarkReport>(
          Status::failure(ErrorCode::Cancelled,
                          "raw benchmark was cancelled between iterations"));
    }

    RawBenchmarkSample sample;
    sample.iteration = iteration;
    auto compiled = compiler_->compile(graph, options.planning);
    if (!compiled.ok()) {
      sample.outcome = compiled.status().code;
      sample.reason = compiled.status().message;
      report.samples.push_back(std::move(sample));
      continue;
    }
    CompiledWorkflow workflow = compiled.take_value();
    sample.compilation = workflow.diagnostics;

    auto executed =
        execution_->execute(workflow.plan, cancellation, options.execution);
    if (!executed.ok()) {
      sample.outcome = executed.status().code;
      sample.reason = executed.status().message;
      report.samples.push_back(std::move(sample));
      continue;
    }
    ExecutionResult execution_result = executed.take_value();
    sample.execution = execution_result.diagnostics;

    if (options.correctness_oracle) {
      sample.correctness_checked = true;
      try {
        CorrectnessObservation observation =
            options.correctness_oracle(execution_result);
        sample.correctness_accepted = observation.accepted;
        sample.reason = std::move(observation.detail);
        if (!sample.correctness_accepted) {
          sample.outcome = ErrorCode::OperationFailed;
        }
      } catch (const std::bad_alloc&) {
        throw;
      } catch (const std::exception& error) {
        sample.outcome = ErrorCode::OperationFailed;
        sample.reason = error.what();
      } catch (...) {
        sample.outcome = ErrorCode::OperationFailed;
        sample.reason = "correctness oracle raised a nonstandard exception";
      }
    }
    report.samples.push_back(std::move(sample));
  }
  return Result<RawBenchmarkReport>(std::move(report));
}

}  // namespace ps
