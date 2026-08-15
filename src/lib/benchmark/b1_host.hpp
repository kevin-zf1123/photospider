/**
 * @file b1_host.hpp
 * @brief Declares the source-private embedded Host seam for B1 evidence.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "compute/compute_run.hpp"  // NOLINT(build/include_subdir)
#include "compute/execution_lifecycle_telemetry.hpp"  // NOLINT(build/include_subdir)
#include "execution/compute_io_executor.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"
#include "runtime/resource_ledger.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

/**
 * @brief Complete source-private B1 compute admission value.
 * @throws Nothing for default destruction; owned request/sink copies may
 * allocate.
 * @note The ordinary request retains installed fields while QoS/observation
 * remain source-private and never cross IPC or installed ABI.
 */
struct B1HostComputeRequest final {
  /** @brief Ordinary Host request selecting session/node/cache/cap. */
  HostComputeRequest request;
  /** @brief Exact Throughput, no-deadline, weight-one, cap-one/eight QoS. */
  compute::ComputeRunQos qos;
  /** @brief Observation-only sink retained by the materialized Run. */
  std::shared_ptr<compute::ComputeRunObservationSink> observation_sink;
};

/**
 * @brief Read-only process execution state retained at one B1 evidence cut.
 * @throws std::bad_alloc when device/lifecycle storage allocates.
 * @note The value contains no ledger token, Run lease, task completion,
 * cancellation source, queue entry, native handle, or Graph authority.
 */
struct B1ExecutionSnapshot final {
  /** @brief Authoritative Host limits/current/lifetime-high-water values. */
  ResourceLedger::Snapshot host_resources;
  /** @brief Deterministically ordered configured device resource accounts. */
  std::vector<ResourceLedger::DeviceSnapshot> device_resources;
  /** @brief Bounded raw product lifecycle evidence page. */
  compute::ExecutionLifecyclePage lifecycle;
  /** @brief Independent process Compute I/O budgets and phase counts. */
  execution::ComputeIoExecutorSnapshot compute_io;
};

/**
 * @brief Source-private B1 capability implemented only by the embedded Host.
 *
 * Product computation remains on the same synchronous embedded Host path; the
 * private request adds only exact Throughput QoS and an observation sink. The
 * seam also exposes the existing process Compute I/O worker to the dedicated
 * output owner and authority-free state snapshots to the collector.
 *
 * @throws As documented by individual methods.
 * @note This interface is neither installed nor exposed through IPC, CLI,
 * plugins, operation callbacks, or provider contracts.
 */
class B1Host {
 public:
  /**
   * @brief Releases the source-private view without controlling product work.
   * @throws Nothing.
   */
  virtual ~B1Host() noexcept = default;

  /**
   * @brief Computes one B1 image through the real synchronous Host path.
   * @param request Ordinary request plus exact private QoS/observer.
   * @return Same typed image/status contract as `Host::compute_and_get_image`.
   * @throws std::bad_alloc when request/evidence ownership cannot allocate.
   * @note This method reuses Host lifecycle admission, InteractionService,
   * Kernel, ExecutionService, providers, ledger, cache, and Run routes; it
   * creates no benchmark execution authority.
   */
  virtual Result<ImageBuffer> compute_b1_image(
      B1HostComputeRequest request) = 0;

  /**
   * @brief Returns the one existing process Compute I/O executor.
   * @return Borrowed executor authority for the B1 `OutputStore` only.
   * @throws Nothing.
   * @note The returned owner has no path/output policy by itself and must not
   * outlive the concrete Host.
   */
  virtual execution::ComputeIoExecutor& b1_compute_io_executor() noexcept = 0;

  /**
   * @brief Copies authoritative execution/resource evidence at one boundary.
   * @param after_cursor Lifecycle cursor strictly before desired events.
   * @param limit Positive bounded maximum lifecycle events to return.
   * @return Host/device/lifecycle/Compute-I/O state without authority.
   * @throws std::invalid_argument when `limit` violates lifecycle bounds.
   * @throws std::bad_alloc or std::system_error from snapshot ownership and
   * synchronization.
   */
  virtual B1ExecutionSnapshot b1_execution_snapshot(
      std::uint64_t after_cursor, std::size_t limit) const = 0;
};

/**
 * @brief Obtains the private B1 capability from one polymorphic Host.
 * @param host Host instance to inspect without transferring ownership.
 * @return Capability pointer, or null for a non-embedded Host.
 * @throws Nothing.
 * @note The pointer remains valid only while `host` lives.
 */
inline B1Host* as_b1_host(Host& host) noexcept {
  return dynamic_cast<B1Host*>(&host);
}

}  // namespace ps::benchmark
