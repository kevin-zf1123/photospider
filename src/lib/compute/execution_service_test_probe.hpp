#pragma once

#include <cstdint>
#include <string>

#include "runtime/resource_ledger.hpp"

namespace ps::compute {
struct OperationExecutionConstraints;
struct ReadyTaskResourceDemand;
}  // namespace ps::compute

namespace ps::compute::testing {

/**
 * @brief Test-product snapshot of two reserved-start attempts.
 *
 * @throws Nothing for value construction and copying.
 * @note This source-tree-private type is referenced only by the separately
 * compiled, non-installed execution-service test product. The production
 * `ExecutionService` class and worker path expose no corresponding member,
 * observer, callback, or state.
 */
struct ReservedStartRollbackProbeSnapshot final {
  /** @brief Number of reserved-start attempts observed while armed. */
  std::uint64_t calls = 0U;

  /** @brief First two candidate identities. */
  std::uint64_t candidate_ids[2] = {0U, 0U};

  /** @brief First two nonreused entry versions. */
  std::uint64_t entry_versions[2] = {0U, 0U};

  /** @brief First two immutable route generations. */
  std::uint64_t route_generations[2] = {0U, 0U};

  /** @brief First two staged execution child-grant vectors. */
  ResourceVector resources[2];
};

/**
 * @brief Arms one allocation-free reserved-start rollback observation.
 * @return Nothing.
 * @throws Nothing.
 * @note Only one isolated test-product service may execute while armed. The
 * first staged grant is discarded by RAII; the next attempt proceeds normally.
 */
void arm_reserved_start_rollback_probe_for_testing() noexcept;

/**
 * @brief Copies the test-product reserved-start observation.
 * @return First two attempts and total observed call count.
 * @throws Nothing.
 */
ReservedStartRollbackProbeSnapshot
reserved_start_rollback_probe_snapshot_for_testing() noexcept;

/**
 * @brief Disarms the test-product reserved-start observation.
 * @return Nothing.
 * @throws Nothing.
 */
void disarm_reserved_start_rollback_probe_for_testing() noexcept;

/**
 * @brief Observes one operation gate denial in the non-installed test product.
 * @param context Opaque fixture state installed before isolated execution.
 * @param implementation_identity Exact denied implementation identity.
 * @return Nothing.
 * @throws Nothing; callbacks must use allocation-free, nonblocking operations.
 * @note Notification occurs with the execution-service pool mutex held. The
 * callback may publish atomic state and notify a condition variable, but must
 * not call back into ExecutionService or acquire a service-owned mutex.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
using OperationAdmissionWaitObserver =
    void (*)(void* context, std::uint64_t implementation_identity) noexcept;
// NOLINTEND

/**
 * @brief Installs one process-local operation-admission denial observer.
 * @param observer Allocation-free callback, or null to disable observation.
 * @param context Opaque callback context, or null when disabling.
 * @return Nothing.
 * @throws Nothing.
 * @note Only one isolated test-product service may execute while installed.
 * The production execution-service translation unit contains no matching
 * observer state, notification branch, declaration, or symbol.
 */
void set_operation_admission_wait_observer_for_testing(
    OperationAdmissionWaitObserver observer, void* context) noexcept;

/**
 * @brief Clears the process-local operation-admission denial observer.
 * @return Nothing.
 * @throws Nothing.
 * @note The owning test first settles all potentially notifying work.
 */
void clear_operation_admission_wait_observer_for_testing() noexcept;

/**
 * @brief Identifies one owned operation-string charge in product estimation.
 *
 * @throws Nothing for value construction and comparison.
 * @note Values are emitted only by the separately compiled, non-installed
 * internal test product. They identify ownership boundaries, not public
 * resource dimensions or operation ABI values.
 */
enum class RetainedOperationStringOwner : std::uint8_t {
  /** @brief Exclusive key retained by one planned operation route. */
  ComputePlanOperationRoute = 0U,
  /** @brief Exclusive key retained by a full-plan resolved implementation. */
  FullPlanResolvedOperation,
  /** @brief Exclusive key retained by a full-plan execution constraint. */
  FullPlanExecutionConstraint,
  /** @brief Exclusive key retained by a dirty resolved implementation map. */
  DirtyResolvedOperation,
  /** @brief Exclusive key retained by one HP dirty execution constraint. */
  DirtyHighPrecisionExecutionConstraint,
  /** @brief Exclusive key retained by one RT dirty execution constraint. */
  DirtyRealTimeExecutionConstraint,
  /** @brief Key retained by a connected-preflight provider callable. */
  ConnectedPreflightOperationConstraint,
  /** @brief Key retained by a connected-preflight ready submission. */
  ConnectedPreflightSubmissionConstraint,
  /** @brief Sentinel count for fixed-size test observation tables. */
  Count,
};

/**
 * @brief Observes one checked charge against an actual owned operation string.
 * @param context Opaque fixture state installed before synchronous estimation.
 * @param owner Exact retained owner category.
 * @param capacity Actual `std::string::capacity()` of the charged owner.
 * @param before_bytes Estimator total immediately before the charge.
 * @param after_bytes Estimator total immediately after the charge.
 * @return Nothing.
 * @throws Nothing; callbacks must use allocation-free, nonblocking operations.
 * @note The callback independently compares the observed capacity with the
 * checked estimator delta. It receives no string pointer or mutable owner.
 */
using RetainedOperationStringChargeObserver = void (*)(
    void* context, RetainedOperationStringOwner owner, std::uint64_t capacity,
    std::uint64_t before_bytes, std::uint64_t after_bytes) noexcept;

/**
 * @brief Installs one process-local retained operation-string observer.
 * @param observer Allocation-free callback, or null to disable observation.
 * @param context Opaque callback context, or null when disabling.
 * @return Nothing.
 * @throws Nothing.
 * @note Only one isolated synchronous product test may estimate while
 * installed. Production translation units contain no matching state or calls.
 */
void set_retained_operation_string_charge_observer_for_testing(
    RetainedOperationStringChargeObserver observer, void* context) noexcept;

/**
 * @brief Clears the process-local retained operation-string observer.
 * @return Nothing.
 * @throws Nothing.
 * @note The owning test first settles every potentially estimating path.
 */
void clear_retained_operation_string_charge_observer_for_testing() noexcept;

/**
 * @brief Publishes one actual owned string and its checked charge interval.
 * @param owner Exact retained owner category.
 * @param value Actual `std::string` whose allocation was just charged.
 * @param before_bytes Estimator total immediately before the charge.
 * @param after_bytes Estimator total immediately after the charge.
 * @return Nothing.
 * @throws Nothing.
 * @note This notification exists only in internal test-product seam objects.
 * It observes the owner's real capacity after production estimation and does
 * not participate in the estimate or grant resource authority.
 */
void notify_retained_operation_string_charge_for_testing(
    RetainedOperationStringOwner owner, const std::string& value,
    std::uint64_t before_bytes, std::uint64_t after_bytes) noexcept;

/**
 * @brief Calculates the production direct-lease resource vector for tests.
 * @param constraints Exact implementation/key declaration copied by a request.
 * @param demand Declared retained/scratch bytes and positive work units.
 * @return CPU, retained-memory, scratch, ready-entry, and ready-byte vector
 * reserved by acquire_operation_execution().
 * @throws GraphError when checked retained-memory arithmetic overflows.
 * @note This diagnostic mints no authority and exists only in the separately
 * compiled, non-installed execution-service test product. It lets a test
 * inject capacity equal to one direct callback without duplicating private
 * ownership-envelope arithmetic. The estimator first copies the supplied
 * constraints exactly as production lease construction does, then charges the
 * copied key's actual capacity plus its null terminator.
 */
ResourceVector estimate_direct_operation_resources_for_testing(
    const OperationExecutionConstraints& constraints,
    ReadyTaskResourceDemand demand);

/**
 * @brief Returns the key-independent retained bytes in one direct lease.
 * @return Checked private lease-state and reservation-state structural bytes.
 * @throws GraphError when checked retained-memory arithmetic overflows.
 * @note This authority-free value exists only in the separately compiled,
 * non-installed execution-service test product. It lets exact-capacity tests
 * independently add the declared bytes and copied string payload without
 * exposing the private lease-state type or layout.
 */
std::uint64_t direct_operation_fixed_retained_memory_bytes_for_testing();

}  // namespace ps::compute::testing
