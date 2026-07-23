#pragma once

#include <cstdint>

#include "runtime/resource_ledger.hpp"

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

}  // namespace ps::compute::testing
