#include "photospider/benchmark/raw_benchmark.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#ifdef PHOTOSPIDER_ENABLE_RAW_BENCHMARK_TEST_HOOKS
#include "benchmark/raw_benchmark_test_hooks.hpp"
#endif

namespace ps {
namespace {

/**
 * @brief Validates one bounded canonical UTF-8 correctness-oracle identity.
 * @param value Candidate identity bytes.
 * @return True for 1..128 bytes of valid non-control UTF-8 excluding the
 * reserved `unchecked` marker and leading/trailing ASCII space.
 * @throws Nothing.
 * @note Validation rejects overlong sequences, surrogates, and code points
 * beyond U+10FFFF; normalization is not performed.
 */
bool valid_oracle_name(const std::string& value) noexcept {
  if (value.empty() || value.size() > 128U || value == "unchecked" ||
      value.front() == ' ' || value.back() == ' ') {
    return false;
  }
  std::size_t index = 0U;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7fU) {
      if (first < 0x20U || first == 0x7fU) {
        return false;
      }
      ++index;
      continue;
    }
    std::size_t width = 0U;
    std::uint32_t code_point = 0U;
    std::uint32_t minimum = 0U;
    if ((first & 0xe0U) == 0xc0U) {
      width = 2U;
      code_point = first & 0x1fU;
      minimum = 0x80U;
    } else if ((first & 0xf0U) == 0xe0U) {
      width = 3U;
      code_point = first & 0x0fU;
      minimum = 0x800U;
    } else if ((first & 0xf8U) == 0xf0U) {
      width = 4U;
      code_point = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (width > value.size() - index) {
      return false;
    }
    for (std::size_t offset = 1U; offset < width; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xc0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3fU);
    }
    if (code_point < minimum || code_point > 0x10ffffU ||
        (code_point >= 0xd800U && code_point <= 0xdfffU)) {
      return false;
    }
    index += width;
  }
  return true;
}

}  // namespace

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
 * @note Test-enabled builds may replace completed compiler and executor
 * diagnostics through a private non-installed seam before sample copying;
 * production builds contain neither the seam implementation nor these calls.
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
  if (options.correctness_oracle) {
    if (!valid_oracle_name(options.oracle_name)) {
      return Result<RawBenchmarkReport>(Status::failure(
          ErrorCode::InvalidArgument,
          "correctness oracle requires a canonical UTF-8 name"));
    }
  } else if (!options.oracle_name.empty()) {
    return Result<RawBenchmarkReport>(
        Status::failure(ErrorCode::InvalidArgument,
                        "oracle name conflicts with an unchecked benchmark"));
  }

  RawBenchmarkReport report;
  report.oracle_name =
      options.correctness_oracle ? options.oracle_name : "unchecked";
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
    sample.oracle_name = report.oracle_name;
    auto compiled = compiler_->compile(graph, options.planning);
    if (!compiled.ok()) {
      sample.outcome = compiled.status().code;
      sample.reason = compiled.status().message;
      report.samples.push_back(std::move(sample));
      continue;
    }
    CompiledWorkflow workflow = compiled.take_value();
#ifdef PHOTOSPIDER_ENABLE_RAW_BENCHMARK_TEST_HOOKS
    benchmark_testing::apply_completed_compilation_hook(workflow.diagnostics);
#endif
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
#ifdef PHOTOSPIDER_ENABLE_RAW_BENCHMARK_TEST_HOOKS
    benchmark_testing::apply_completed_execution_hook(
        execution_result.diagnostics);
#endif
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
