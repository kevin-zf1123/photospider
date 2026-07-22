#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ps::execution {

/**
 * @brief Host-owned state for the three private physical execution routes.
 *
 * The owner centralizes route discovery plus allocation-free start/finish
 * admission. CPU and GPU-pipeline work may overlap on the process worker pool;
 * serial-debug work is restricted to worker zero and one in-flight callback.
 * No route is installed, exported, or supplied by a policy DSO.
 *
 * @throws Nothing for state transitions. Copied discovery/description values
 * may throw `std::bad_alloc`.
 * @note Every instance is owned by one `ExecutionService` and is accessed only
 * while that service's ready-store mutex is held. The type therefore owns no
 * mutex and exposes no public or plugin ABI.
 */
class PhysicalExecutionRoutes final {
 public:
  /**
   * @brief Copies the complete canonical private route inventory.
   * @return Exactly `cpu`, `gpu_pipeline`, and `serial_debug` in lexical order.
   * @throws std::bad_alloc when result storage cannot allocate.
   */
  static std::vector<std::string> available_types();

  /**
   * @brief Copies the reader-facing description for one private route.
   * @param type_name Exact canonical route name.
   * @return Owned route description.
   * @throws GraphError with `GraphErrc::NotFound` for an unknown or removed
   * route name.
   * @throws std::bad_alloc when result or diagnostic storage cannot allocate.
   */
  static std::string description(std::string_view type_name);

  /**
   * @brief Reports whether one name denotes a private physical route.
   * @param type_name Candidate route name.
   * @return True only for `cpu`, `gpu_pipeline`, or `serial_debug`.
   * @throws Nothing.
   */
  static bool is_supported(std::string_view type_name) noexcept;

  /**
   * @brief Tests route-local start constraints without changing state.
   * @param type_name Exact route selected by the Graph binding.
   * @param worker_id Stable zero-based process worker id.
   * @param run_in_flight Number of callbacks already entered for this Run.
   * @return True when this route can accept the callback now.
   * @throws Nothing.
   * @note Run cancellation, maximum parallelism, resource grants, and policy
   * admissibility are validated by `ExecutionService` before this route-local
   * check. Serial-debug additionally requires worker zero, an idle global
   * serial route, and no callback already in flight for the same Run.
   */
  bool can_start(std::string_view type_name, int worker_id,
                 std::uint64_t run_in_flight) const noexcept;

  /**
   * @brief Commits one route-local callback start.
   * @param type_name Exact route already accepted by `can_start()`.
   * @return True after incrementing the matching in-flight counter; false when
   * the route became unavailable, shutdown began, or its counter is exhausted.
   * @throws Nothing.
   * @note Caller serialization makes the check/commit pair atomic with the
   * ready-store reserved-start transition. No allocation or external callback
   * occurs here.
   */
  bool commit_start(std::string_view type_name) noexcept;

  /**
   * @brief Retires one previously committed route-local callback.
   * @param type_name Exact route whose callback exited or was suppressed.
   * @return True after decrementing the matching counter; false for an unknown
   * route or unmatched finish.
   * @throws Nothing.
   */
  bool finish(std::string_view type_name) noexcept;

  /**
   * @brief Stops future starts while allowing committed callbacks to retire.
   * @return Nothing.
   * @throws Nothing.
   * @note The transition is idempotent. `ExecutionService` invokes it before
   * waking and joining its physical workers.
   */
  void begin_shutdown() noexcept { stopping_ = true; }

  /**
   * @brief Reports whether every committed route callback has retired.
   * @return True only when all three in-flight counters are zero.
   * @throws Nothing.
   */
  bool drained() const noexcept;

  /**
   * @brief Returns one route's current in-flight count for repository tests.
   * @param type_name Exact route name.
   * @return Matching count, or zero for an unknown route.
   * @throws Nothing.
   * @note This diagnostic grants no start, worker, queue, or lifecycle
   * authority.
   */
  std::uint64_t in_flight(std::string_view type_name) const noexcept;

 private:
  /** @brief Committed process CPU-route callbacks. */
  std::uint64_t cpu_in_flight_ = 0U;

  /** @brief Committed Host GPU-pipeline-route callbacks. */
  std::uint64_t gpu_pipeline_in_flight_ = 0U;

  /** @brief Committed deterministic serial-debug callbacks. */
  std::uint64_t serial_debug_in_flight_ = 0U;

  /** @brief True after service teardown closes physical route admission. */
  bool stopping_ = false;
};

}  // namespace ps::execution
