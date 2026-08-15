/**
 * @file m1_host.hpp
 * @brief Declares the source-private embedded Host seam for M1 diagnostics.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "compute/execution/execution_lifecycle_telemetry.hpp"  // NOLINT(build/include_subdir)
#include "compute/execution/execution_service.hpp"  // NOLINT(build/include_subdir)
#include "execution/device/compute_io_executor.hpp"  // NOLINT(build/include_subdir)
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
 * Its components are independently synchronized copies rather than one
 * cross-owner transaction; a live harness retries or uses a settled boundary
 * before asserting a relation across owners.
 */
struct M1ExecutionSnapshot final {
  /** @brief Authoritative limits/current/lifetime Host resource values. */
  ResourceLedger::Snapshot host_resources;

  /** @brief Every configured authoritative device resource account. */
  std::vector<ResourceLedger::DeviceSnapshot> device_resources;

  /** @brief Sparse process Compute I/O current/phase diagnostic cut only. */
  execution::ComputeIoExecutorSnapshot compute_io;

  /** @brief Fixed general capacity and active Throughput root total. */
  compute::ExecutionThroughputReservationSnapshot throughput;

  /** @brief Real queued entries partitioned by immutable product QoS class. */
  compute::ExecutionReadyClassSnapshot ready_classes;

  /** @brief Bounded raw product lifecycle evidence page. */
  compute::ExecutionLifecyclePage lifecycle;

  /** @brief Exact lifecycle cursor supplied to this snapshot request. */
  std::uint64_t lifecycle_after_cursor = 0U;

  /**
   * @brief Chronological M1 capture coordinate assigned by the runner.
   * @note The Host returns the sentinel because it does not own protocol
   * phase order; the runner must replace it before retaining row evidence.
   */
  std::size_t temporal_capture_ordinal =
      std::numeric_limits<std::size_t>::max();
};

/**
 * @brief Source-private M1 evidence capability of the embedded Host.
 *
 * Computation remains exclusively on `I1Host` and `B1Host`. This interface
 * composes read-only evidence samples from the same process-owned execution
 * service and exposes one terminal shutdown operation so the manual runner can
 * retain the producer's final-zero ServiceStopped cut.
 *
 * @throws As documented by each method.
 * @note This interface is neither installed nor exposed through IPC, CLI,
 * plugins, providers, or policy contracts. Shutdown is not a benchmark phase
 * control: it is permitted only after every runner-owned Graph has closed.
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
   * @return Host/device ledgers, Compute I/O, ready/Throughput, lifecycle
   * diagnostics, the exact requested cursor, and an unassigned capture ordinal.
   * @throws std::invalid_argument when `limit` violates lifecycle bounds.
   * @throws std::bad_alloc or std::system_error from snapshot ownership and
   * synchronization.
   * @note Observation neither waits for quiescence nor changes any counter;
   * component samples are not one cross-owner atomic transaction. The protocol
   * runner owns temporal order and must replace the capture-ordinal sentinel
   * before retaining this snapshot as row evidence.
   */
  virtual M1ExecutionSnapshot m1_execution_snapshot(
      std::uint64_t after_cursor, std::size_t limit) const = 0;

  /**
   * @brief Irreversibly settles the process execution domain for final proof.
   * @return Nothing after idempotent ExecutionService shutdown completes.
   * @throws std::logic_error when a Host Graph, synchronous admission, or
   * asynchronous compute remains owned by the embedded adapter.
   * @throws ExecutionService shutdown synchronization failures unchanged.
   * @note The runner must call this only after closing all Graph sessions and
   * before its final `m1_execution_snapshot`. No later Host computation or
   * Graph load is permitted; repeated calls are idempotent.
   */
  virtual void m1_shutdown_execution() = 0;
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
