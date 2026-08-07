/**
 * @file m1_host.hpp
 * @brief Declares the source-private embedded Host seam for M1 diagnostics.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "compute/execution_lifecycle_telemetry.hpp"  // NOLINT(build/include_subdir)
#include "compute/execution_service.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"
#include "runtime/resource_ledger.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

/**
 * @brief Read-only process execution state retained at one M1 boundary.
 *
 * The value joins the authoritative all-class Host ledger with policy-only
 * Throughput capacity/reserved diagnostics and bounded lifecycle evidence.
 * It deliberately contains no compute method because M1 reuses the accepted
 * `I1Host` and `B1Host` execution seams.
 *
 * @throws std::bad_alloc when lifecycle snapshot storage allocates.
 * @note This aggregate contains no ledger token, reservation, grant, Run
 * lease, queue entry, cancellation source, native handle, or policy authority.
 * Its three components are independently synchronized copies rather than one
 * cross-owner transaction; a live harness retries or uses a settled boundary
 * before asserting a relation across components.
 */
struct M1ExecutionSnapshot final {
  /** @brief Authoritative limits/current/lifetime Host resource values. */
  ResourceLedger::Snapshot host_resources;

  /** @brief Fixed general capacity and active Throughput root total. */
  compute::ExecutionThroughputReservationSnapshot throughput;

  /** @brief Bounded raw product lifecycle evidence page. */
  compute::ExecutionLifecyclePage lifecycle;
};

/**
 * @brief Source-private M1 diagnostic capability of the embedded Host.
 *
 * Computation remains exclusively on `I1Host` and `B1Host`. This interface
 * only composes read-only evidence samples from the same process-owned
 * execution service.
 *
 * @throws As documented by `m1_execution_snapshot`.
 * @note This interface is neither installed nor exposed through IPC, CLI,
 * plugins, providers, or policy contracts.
 */
class M1Host {
 public:
  /**
   * @brief Releases the source-private view without controlling product work.
   * @throws Nothing.
   */
  virtual ~M1Host() noexcept = default;

  /**
   * @brief Copies authoritative and policy-only state at one M1 boundary.
   * @param after_cursor Lifecycle cursor strictly before desired events.
   * @param limit Positive bounded maximum lifecycle events to return.
   * @return Host ledger, Throughput account, and lifecycle diagnostics.
   * @throws std::invalid_argument when `limit` violates lifecycle bounds.
   * @throws std::bad_alloc or std::system_error from snapshot ownership and
   * synchronization.
   * @note Observation neither waits for quiescence nor changes any counter;
   * component samples are not one cross-owner atomic transaction.
   */
  virtual M1ExecutionSnapshot m1_execution_snapshot(
      std::uint64_t after_cursor, std::size_t limit) const = 0;
};

/**
 * @brief Obtains the private M1 diagnostic capability from a Host.
 * @param host Host instance to inspect without transferring ownership.
 * @return Capability pointer, or null for a non-embedded Host.
 * @throws Nothing.
 * @note The pointer remains valid only while `host` lives.
 */
inline M1Host* as_m1_host(Host& host) noexcept {
  return dynamic_cast<M1Host*>(&host);
}

}  // namespace ps::benchmark
