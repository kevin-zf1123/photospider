#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/execution.hpp"

namespace ps {

/**
 * @brief Correctness observation returned by a benchmark oracle.
 *
 * @note This is a local test observation, not an attestation or release gate.
 */
struct PHOTOSPIDER_API CorrectnessObservation final {
  /** @brief Whether the produced named Values satisfy the caller oracle. */
  bool accepted = false;
  /** @brief Optional human-readable mismatch detail. */
  std::string detail;
};

/**
 * @brief Caller-supplied pure observation over one successful execution.
 *
 * @note The callback receives immutable Values and should avoid modifying
 * external state when reproducible measurements are required. It does not
 * receive a CancellationToken and cannot be preempted; the runner observes
 * cancellation immediately before and after the callback.
 */
using OracleSignature = CorrectnessObservation(const ExecutionResult&);
/** @brief Type-erased callable implementing `OracleSignature`. */
using CorrectnessOracle = std::function<OracleSignature>;

/**
 * @brief Controls one local raw benchmark run.
 *
 * @note The type contains no performance threshold or deployment decision.
 */
struct PHOTOSPIDER_API RawBenchmarkOptions final {
  /** @brief Positive number of independently compiled/executed samples. */
  std::uint32_t iterations = 1U;
  /** @brief Physical planning options repeated for every sample. */
  PlanningOptions planning;
  /** @brief Local execution parallelism repeated for every sample. */
  ExecutionOptions execution;
  /** @brief Optional correctness observation run after successful execution. */
  CorrectnessOracle correctness_oracle;
  /**
   * @brief Bounded canonical UTF-8 identity required with an oracle.
   *
   * Leave empty only when `correctness_oracle` is absent; such a run is
   * reported explicitly as `unchecked`.
   */
  std::string oracle_name;
};

/**
 * @brief Raw measurement and outcome for one benchmark iteration.
 *
 * @note A non-cancellation failure preserves completed compiler timing and a
 * typed error reason. Cancellation observed before final report publication
 * aborts the complete run instead of publishing any sample sequence.
 */
struct PHOTOSPIDER_API RawBenchmarkSample final {
  /** @brief Zero-based iteration index. */
  std::uint32_t iteration = 0U;
  /** @brief Raw typed-compiler stage timings. */
  CompilationDiagnostics compilation;
  /** @brief Raw execution diagnostics, present as defaults before execution. */
  ExecutionDiagnostics execution;
  /** @brief Success or first compiler/execution/oracle exception category. */
  ErrorCode outcome = ErrorCode::Ok;
  /**
   * @brief Human-readable failure or oracle detail.
   * @note A standard oracle exception with a null diagnostic produces an
   * empty reason while retaining the typed `OperationFailed` sample.
   */
  std::string reason;
  /** @brief Whether a correctness oracle was invoked. */
  bool correctness_checked = false;
  /** @brief Oracle observation when checked. */
  bool correctness_accepted = false;
  /** @brief Canonical oracle identity or the explicit value `unchecked`. */
  std::string oracle_name;
};

/**
 * @brief Complete sequence of local raw benchmark samples.
 *
 * @note Reports are ordinary in-memory diagnostics with no durable identity.
 */
struct PHOTOSPIDER_API RawBenchmarkReport final {
  /** @brief Canonical oracle identity or `unchecked`, shared by every sample.
   */
  std::string oracle_name;
  /**
   * @brief Samples in increasing iteration order.
   * @note No report is published when cancellation is observed before the
   * runner's final publication decision.
   */
  std::vector<RawBenchmarkSample> samples;
};

/**
 * @brief Runs repeated compile-plan-execute measurements over local resources.
 *
 * @note Compiler and ExecutionContext ownership remains with the caller and
 * must outlive this runner plus every `run` call.
 */
class PHOTOSPIDER_API RawBenchmarkRunner final {
 public:
  /**
   * @brief Binds one compiler and local execution context.
   * @param compiler Nonnull typed compiler.
   * @param execution Nonnull local execution resource context.
   * @throws std::invalid_argument If either pointer is null.
   * @note No work starts during construction.
   */
  RawBenchmarkRunner(Compiler* compiler, ExecutionContext* execution);

  /**
   * @brief Repeats compile-plan-execute and optional correctness observation.
   * @param graph Independently owned source graph context.
   * @param options Positive iteration and local execution controls.
   * @param cancellation Cooperative cancellation shared by all iterations.
   * @return Complete raw report, invalid setup failure, or top-level
   * `Cancelled` when cancellation is observed before the final report
   * publication decision.
   * @throws std::bad_alloc If report or pipeline allocation fails.
   * @note Compiler and non-cancellation execution failures become sample
   * outcomes and later iterations continue. An execution `Cancelled` result
   * aborts immediately. The caller oracle receives no token and may drain, but
   * cancellation observed before it starts or after it returns/raises an
   * ordinary exception takes precedence over its outcome. A standard oracle
   * exception otherwise becomes an `OperationFailed` sample; a null
   * diagnostic is normalized to an empty reason and later iterations
   * continue. The final token observation is the report-publication
   * linearization point; cancellation after that observation does not
   * retroactively revoke the returned report.
   */
  [[nodiscard]] Result<RawBenchmarkReport> run(
      const GraphContext& graph, const RawBenchmarkOptions& options,
      const CancellationToken& cancellation = CancellationToken()) const;

 private:
  /** @brief Caller-owned typed compiler. */
  Compiler* compiler_;
  /** @brief Caller-owned local execution context. */
  ExecutionContext* execution_;
};

}  // namespace ps
