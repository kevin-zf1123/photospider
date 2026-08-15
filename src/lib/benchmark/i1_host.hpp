/**
 * @file i1_host.hpp
 * @brief Declares the source-private embedded Host seam for I1 verification.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <vector>

#include "compute/compute_run.hpp"  // NOLINT(build/include_subdir)
#include "compute/execution_lifecycle_telemetry.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"
#include "runtime/resource_ledger.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

/**
 * @brief Complete private admission value for one I1 edit.
 *
 * The public request retains ordinary session/node/cache/intent/Region fields.
 * QoS and observation remain source-private so the benchmark exercises the
 * product Host boundary without adding a profile selector or borrowed sink to
 * the installed copyable request.
 *
 * @throws Nothing for default destruction; member copies may allocate.
 * @note The shared observer owns evidence only. It grants no scheduling,
 * cancellation, ledger, graph, or commit authority.
 */
struct I1HostComputeRequest final {
  /** @brief Ordinary installed Host request translated by the embedded Host. */
  HostComputeRequest request;

  /** @brief Exact Interactive/weight/deadline/cap inputs for the product Run.
   */
  compute::ComputeRunQos qos;

  /** @brief Observation-only sink retained by every materialized child Run. */
  std::shared_ptr<compute::ComputeRunObservationSink> observation_sink;

  /**
   * @brief Pre-call row-local coordinate proposed for success-only binding.
   * @note The embedded Host carries this value into product supersession. A
   * failed Host scheduling return does not make it accepted or current.
   */
  compute::AcceptedBoundaryCoordinate accepted_coordinate;
};

/**
 * @brief Read-only process execution snapshot used at I1 drain boundaries.
 * @throws std::bad_alloc when lifecycle/device snapshot storage allocates.
 * @note The value contains no ledger token, Run lease, cancellation source,
 * native handle, queue entry, or graph-state authority.
 */
struct I1ExecutionSnapshot final {
  /** @brief Authoritative Host limits/current/lifetime-high-water values. */
  ResourceLedger::Snapshot host_resources;

  /** @brief Deterministically ordered configured device resource accounts. */
  std::vector<ResourceLedger::DeviceSnapshot> device_resources;

  /** @brief Bounded lifecycle event/counter page for the requested cursor. */
  compute::ExecutionLifecyclePage lifecycle;
};

/**
 * @brief Source-private I1 capability implemented only by the embedded Host.
 *
 * The interface reuses the exact Host lifecycle admission, asynchronous
 * tracking, InteractionService, Kernel, supersession, and ExecutionService
 * route. It adds only private QoS/evidence inputs and read-only drain
 * snapshots.
 *
 * @throws As documented by individual methods.
 * @note This interface is neither installed nor an IPC/CLI/frontend contract.
 * It owns no backend state independently from the concrete Host implementation.
 */
class I1Host {
 public:
  /**
   * @brief Releases the source-private view without controlling product work.
   * @throws Nothing.
   */
  virtual ~I1Host() noexcept = default;

  /**
   * @brief Admits one I1 edit through the real asynchronous Host path.
   * @param request Complete ordinary Host request plus private QoS/observer.
   * @return Public-style scheduling result and future exact operation status.
   * @throws std::bad_alloc when admission/tracking ownership cannot allocate.
   * @note Returning a successful scheduling result is the I1 Host-acceptance
   * boundary. Caller publication, success-result ownership, status worker, and
   * close tracking are prepared before Kernel can bind the proposed coordinate;
   * a failed return therefore creates no current product identity. After Kernel
   * acceptance, only a no-fail delivery publishes the prebuilt result. The
   * future reports later product execution settlement only.
   */
  virtual Result<std::future<OperationStatus>> compute_i1_async(
      I1HostComputeRequest request) = 0;

  /**
   * @brief Copies authoritative execution/resource evidence at one boundary.
   * @param after_cursor Lifecycle cursor strictly before desired events.
   * @param limit Positive bounded maximum lifecycle events to return.
   * @return Host/device resource values and one lifecycle telemetry page.
   * @throws std::invalid_argument when `limit` violates lifecycle bounds.
   * @throws std::bad_alloc or std::system_error from snapshot ownership and
   * synchronization.
   * @note Observation neither waits for quiescence nor changes any counter.
   */
  virtual I1ExecutionSnapshot i1_execution_snapshot(
      std::uint64_t after_cursor, std::size_t limit) const = 0;
};

/**
 * @brief Obtains the private I1 capability from a polymorphic Host.
 * @param host Host instance to inspect without transferring ownership.
 * @return Private capability pointer, or null for a non-embedded Host.
 * @throws Nothing.
 * @note The pointer remains valid only while `host` lives and grants no extra
 * capability beyond the concrete object's private implementation.
 */
inline I1Host* as_i1_host(Host& host) noexcept {
  return dynamic_cast<I1Host*>(&host);
}

}  // namespace ps::benchmark
