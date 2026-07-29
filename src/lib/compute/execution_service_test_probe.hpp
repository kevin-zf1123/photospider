#pragma once

#include <cstdint>

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
