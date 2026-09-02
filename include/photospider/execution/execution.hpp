#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/core/status.hpp"
#include "photospider/data/value.hpp"
#include "photospider/execution/cancellation.hpp"

namespace ps {

/**
 * @brief Fixed local resource configuration for one ExecutionContext.
 *
 * @note Configuration has process-local meaning and contains no daemon quota or
 * remote capacity.
 */
struct PHOTOSPIDER_API ExecutionContextConfig final {
  /** @brief Fixed CPU worker count; zero resolves to bounded hardware count. */
  std::uint32_t cpu_workers = 0;
  /** @brief Whether one optional local GPU lane is present. */
  bool gpu_enabled = false;
  /**
   * @brief Single aggregate waiting-callback limit across CPU/GPU lanes.
   * @note A callback releases its slot when a worker starts it; running
   * callbacks do not consume this ExecutionContext-wide bound.
   */
  std::uint32_t maximum_queued_tasks = 1024;
  /** @brief Maximum concurrently reserved modeled bytes. */
  std::uint64_t maximum_live_bytes = 256U * 1024U * 1024U;
};

/**
 * @brief Per-execution scheduling controls.
 *
 * @note A zero parallelism resolves to the ExecutionContext CPU worker count.
 */
struct PHOTOSPIDER_API ExecutionOptions final {
  /** @brief Maximum in-flight plan steps for this Run. */
  std::uint32_t maximum_parallelism = 0;
};

/**
 * @brief Raw timing and outcome for one physical operation attempt.
 *
 * @note A GPU rejection followed by CPU fallback produces two attempt records.
 */
struct PHOTOSPIDER_API OperationTiming final {
  /** @brief Stable source node id. */
  std::uint64_t node_id = 0;
  /** @brief Attempted local physical backend. */
  Backend backend = Backend::Cpu;
  /** @brief Monotonic callback duration in microseconds. */
  std::uint64_t duration_us = 0;
  /** @brief Attempt outcome category. */
  ErrorCode outcome = ErrorCode::Ok;
};

/**
 * @brief Raw local execution diagnostics.
 *
 * @note Diagnostics are observations, not verdicts, attestations, or receipts.
 */
struct PHOTOSPIDER_API ExecutionDiagnostics final {
  /** @brief Total execute call duration in microseconds. */
  std::uint64_t execute_us = 0;
  /** @brief Successful physical backend per source node. */
  std::map<std::uint64_t, Backend> selected_backends;
  /** @brief Number of explicit cross-backend input transfers. */
  std::uint64_t transfer_count = 0;
  /** @brief Sum of copied input bytes for explicit transfers. */
  std::uint64_t transfer_bytes = 0;
  /** @brief Peak modeled bytes reserved by the shared local ledger. */
  std::uint64_t peak_live_bytes = 0;
  /** @brief Human-readable CPU fallback reasons in occurrence order. */
  std::vector<std::string> fallback_reasons;
  /** @brief Raw physical callback attempts. */
  std::vector<OperationTiming> operation_timings;
  /** @brief Non-security digest of the executed physical plan. */
  std::string plan_digest;
  /** @brief Non-security digest of named result bytes. */
  std::string result_digest;
};

/**
 * @brief Complete in-memory named execution result.
 *
 * @note Results have no durable identity, retention, receipt, or recovery
 * semantics.
 */
struct PHOTOSPIDER_API ExecutionResult final {
  /** @brief Sorted caller-requested named Values. */
  std::map<std::string, Value> values;
  /** @brief Raw compiler-independent execution diagnostics. */
  ExecutionDiagnostics diagnostics;
};

/**
 * @brief Explicit owner of bounded local CPU/GPU execution resources.
 *
 * @note Independent contexts may run concurrently. Destruction requests stop,
 * rejects queued callbacks, releases their shared waiting admissions, and
 * joins every owned worker thread.
 */
class PHOTOSPIDER_API ExecutionContext final {
 public:
  /**
   * @brief Creates fixed workers, per-lane FIFOs, and shared admission owners.
   * @param operations Frozen operation registry retained for all Runs.
   * @param config Fixed local resource configuration.
   * @throws std::invalid_argument If registry/config is invalid.
   * @throws std::bad_alloc If worker/queue state allocation fails.
   * @note CPU execution is always created; GPU is optional. Both lanes consume
   * the single `maximum_queued_tasks` waiting bound.
   */
  explicit ExecutionContext(std::shared_ptr<OperationRegistry> operations,
                            ExecutionContextConfig config = {});

  /**
   * @brief Stops admission, joins workers, and verifies resource settlement.
   * @throws Nothing.
   * @note Callers must not invoke `execute` concurrently with destruction.
   */
  ~ExecutionContext() noexcept;

  /**
   * @brief Forbids copying owned worker pools and resource accounting.
   * @param other Source context that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Construct a separate context for independent resource ownership.
   */
  ExecutionContext(const ExecutionContext& other) = delete;
  /**
   * @brief Forbids copy assignment of active local execution resources.
   * @param other Source context that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note In-flight Runs and queue ownership are never rebound.
   */
  ExecutionContext& operator=(const ExecutionContext& other) = delete;
  /**
   * @brief Forbids moving worker/resource ownership after construction.
   * @param other Source context that cannot be moved.
   * @throws Nothing; the operation is deleted.
   * @note Stable context lifetime bounds every private ExecutionRun.
   */
  ExecutionContext(ExecutionContext&& other) = delete;
  /**
   * @brief Forbids move assignment of worker pools and registry identity.
   * @param other Source context that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Destruction remains the only worker-ownership teardown path.
   */
  ExecutionContext& operator=(ExecutionContext&& other) = delete;

  /**
   * @brief Executes one validated plan through bounded local resources.
   * @param plan Immutable physical plan for one graph revision.
   * @param cancellation Cooperative cancellation observation.
   * @param options Per-Run parallelism controls.
   * @return Named result or typed cancellation/stale/backend/resource failure.
   * @throws std::bad_alloc If staging/result allocation fails before a
   * recoverable status can be built.
   * @note After every completion and after complete final result/digest/timing
   * assembly, publication rechecks cancellation before plan currentness under
   * the Run mutex. Passing that last check is the sole success-publication
   * linearization point; rejected local output is discarded.
   */
  [[nodiscard]] Result<ExecutionResult> execute(
      const ExecutionPlan& plan,
      const CancellationToken& cancellation = CancellationToken(),
      const ExecutionOptions& options = {});

  /**
   * @brief Returns the fixed resolved CPU worker count.
   * @return Positive worker count.
   * @throws Nothing.
   * @note The value never changes during context lifetime.
   */
  [[nodiscard]] std::uint32_t cpu_workers() const noexcept;

  /**
   * @brief Reports whether the optional local GPU lane exists.
   * @return Configured availability.
   * @throws Nothing.
   * @note Availability does not imply every operation supports GPU.
   */
  [[nodiscard]] bool gpu_enabled() const noexcept;

 private:
  /** @brief Opaque pools, shared waiting admission, and resource ledger. */
  struct Impl;
  /** @brief Unique local execution ownership. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps
