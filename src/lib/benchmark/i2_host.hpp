/**
 * @file i2_host.hpp
 * @brief Declares the source-private embedded Host seam for I2 verification.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>

#include "benchmark/i1_host.hpp"              // NOLINT(build/include_subdir)
#include "compute/execution_service.hpp"      // NOLINT(build/include_subdir)
#include "compute/progressive_compute.hpp"    // NOLINT(build/include_subdir)
#include "execution/compute_io_executor.hpp"  // NOLINT(build/include_subdir)
#include "execution/device_executor_registry.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"

namespace ps::benchmark {

/**
 * @brief Complete private admission value for one I2 progressive edit.
 *
 * @throws Nothing for default destruction; member copies may allocate.
 * @note Preview/final policy and evidence remain source-private. The installed
 * Host request contains only its ordinary realtime intent and dirty Region.
 */
struct I2HostComputeRequest final {
  /** @brief Ordinary installed Host request translated by embedded Host. */
  HostComputeRequest request;

  /** @brief Exact Interactive QoS for the RT preview child. */
  compute::ComputeRunQos preview_qos;

  /** @brief Exact Interactive QoS for the HP final child. */
  compute::ComputeRunQos final_qos;

  /** @brief Observation-only sink shared by both child Runs. */
  std::shared_ptr<compute::ComputeRunObservationSink> observation_sink;

  /** @brief Pre-call row-local coordinate proposed for success-only binding. */
  compute::AcceptedBoundaryCoordinate accepted_coordinate;
};

/**
 * @brief Immutable child lineage required for one explicit device acquisition.
 *
 * @throws Nothing for value construction and copying.
 * @note These scalars are copied from the exact visible child descriptor. They
 * grant no Run lease, cancellation, currentness, scheduler, or native handle.
 */
struct I2ValueLineage final {
  /** @brief Nonzero live Graph instance identity. */
  std::uint64_t graph_instance_id = 0U;

  /** @brief Canonical realtime request target. */
  int target_node_id = -1;

  /** @brief Canonical request intent shared by preview and final children. */
  ComputeIntent request_intent = ComputeIntent::RealTimeUpdate;

  /** @brief Nonzero accepted supersession generation. */
  std::uint64_t supersession_generation = 0U;

  /** @brief Nonzero exact visible child Run identity. */
  std::uint64_t run_id = 0U;
};

/**
 * @brief One explicit Host or device access observation.
 *
 * @throws Nothing for default destruction; optional Value-plan storage is
 * allocation-free.
 * @note The record contains identity and byte facts only. It retains no Value,
 * ReadLease, native object, transfer task, or resource authority.
 */
struct I2ValueAccessEvidence final {
  /** @brief Classified explicit access plan. */
  std::optional<AccessPlan> plan;

  /** @brief Logical Value revision observed at the access boundary. */
  ValueRevisionId revision;

  /** @brief Complete immutable physical binding facts. */
  StorageBinding binding;

  /** @brief Allocation identity repeated for closed-schema convenience. */
  AllocationIdentity allocation;

  /** @brief Exact storage/read envelope in bytes. */
  std::size_t storage_bytes = 0U;

  /** @brief Whether this acquisition entered a concrete executor. */
  bool executor_submitted = false;
};

/**
 * @brief Conditional Metal acquisition evidence for one visible I2 Value.
 *
 * @throws std::bad_alloc when unavailable-reason storage allocates.
 * @note When `available` is false, all optional native observations are empty
 * and only the frozen reason is meaningful. No field carries a native handle.
 */
struct I2MetalAcquisitionEvidence final {
  /** @brief Whether a usable process-owned Metal executor existed. */
  bool available = false;

  /** @brief Frozen N/A reason when Metal is unavailable. */
  std::string unavailable_reason;

  /** @brief Diagnostics before the first process residency lookup. */
  std::optional<execution::DeviceExecutorDiagnostics> before;

  /** @brief First acquisition, which must explicitly transfer on a miss. */
  std::optional<I2ValueAccessEvidence> first;

  /** @brief Diagnostics immediately after the first acquisition settles. */
  std::optional<execution::DeviceExecutorDiagnostics> after_first;

  /** @brief Second acquisition, which must reuse the same resident binding. */
  std::optional<I2ValueAccessEvidence> second;

  /** @brief Diagnostics after the second residency lookup. */
  std::optional<execution::DeviceExecutorDiagnostics> after_second;

  /** @brief Device ledger before first acquisition. */
  std::optional<ResourceLedger::DeviceSnapshot> resources_before;

  /** @brief Device ledger after first transfer and scratch settlement. */
  std::optional<ResourceLedger::DeviceSnapshot> resources_after_first;

  /**
   * @brief Device ledger after second reuse and before exact row release.
   */
  std::optional<ResourceLedger::DeviceSnapshot> resources_after_second;
};

/**
 * @brief Closed Host/conditional-Metal acquisition evidence for one I2 Value.
 *
 * @throws std::bad_alloc when nested optional/string ownership allocates.
 * @note The caller supplies one immutable visible Value. This record releases
 * every payload/read owner and any conditional exact Metal resident before
 * return and therefore cannot extend product currentness, device reservation,
 * or lifecycle settlement.
 */
struct I2ValueAcquisitionEvidence final {
  /** @brief First direct Host read acquisition. */
  I2ValueAccessEvidence host_first;

  /** @brief Second direct Host read acquisition. */
  I2ValueAccessEvidence host_second;

  /** @brief Conditional process Metal transfer/reuse evidence. */
  I2MetalAcquisitionEvidence metal;

  /** @brief Independent compute-I/O counters before any acquisition. */
  execution::ComputeIoExecutorSnapshot io_before;

  /** @brief Independent compute-I/O counters after every acquisition. */
  execution::ComputeIoExecutorSnapshot io_after;
};

/**
 * @brief Source-private I2 capability implemented only by embedded Host.
 *
 * @throws As documented by individual methods.
 * @note The capability reuses the real Host lifecycle, Kernel, execution
 * service, device registry, residency manager, and resource ledger. It is not
 * installed and owns no parallel product authority.
 */
class I2Host {
 public:
  /**
   * @brief Releases the source-private view without controlling product work.
   * @throws Nothing.
   */
  virtual ~I2Host() noexcept = default;

  /**
   * @brief Admits one progressive edit through the real asynchronous Host path.
   * @param request Ordinary realtime request plus private child QoS/evidence.
   * @return Public-style scheduling result and future exact operation status.
   * @throws std::bad_alloc when admission/tracking ownership cannot allocate.
   * @note A successful return is the sole accepted-boundary publication. Both
   * children receive the same accepted coordinate/generation and distinct QoS.
   */
  virtual Result<std::future<OperationStatus>> compute_i2_async(
      I2HostComputeRequest request) = 0;

  /**
   * @brief Captures two Host and conditional two Metal acquisitions.
   * @param value Exact Ready preview or final immutable Value captured while
   * visible; it may be historical when a newer generation is already current.
   * @param lineage Exact visible child descriptor lineage.
   * @param capture_deadline Exclusive absolute episode capture deadline. Every
   * direct traversal and conditional Metal wait uses this same value.
   * @return Closed direct/transfer/reuse/resource/no-I/O evidence.
   * @throws Validation, ReadyFence, native executor, resource, allocation, and
   * synchronization failures unchanged.
   * @throws std::runtime_error when the deadline is expired or tied before a
   * new access, or when one in-flight Metal transfer reaches it.
   * @note This verification-only method executes no Graph work, readback,
   * filesystem, codec, cache, output-store, or document persistence. After
   * copying second-reuse facts it removes only the exact acquired Metal
   * revision/binding/producer. Historical acquisition validates a live managed
   * lineage without changing currentness, so row cleanup changes no ordinary
   * residency lookup, publication, replacement, or capacity policy.
   */
  virtual I2ValueAcquisitionEvidence acquire_i2_value(
      Value value, const I2ValueLineage& lineage,
      std::chrono::steady_clock::time_point capture_deadline) = 0;

  /**
   * @brief Copies authoritative process execution/resource evidence.
   * @param after_cursor Lifecycle cursor strictly before desired events.
   * @param limit Positive bounded maximum lifecycle events to return.
   * @return Host/device resource values and one lifecycle telemetry page.
   * @throws std::invalid_argument for an excessive lifecycle page limit.
   * @throws std::bad_alloc or std::system_error from copied observations.
   */
  virtual I1ExecutionSnapshot i2_execution_snapshot(
      std::uint64_t after_cursor, std::size_t limit) const = 0;
};

/**
 * @brief Obtains the private I2 capability from a polymorphic Host.
 * @param host Host instance to inspect without transferring ownership.
 * @return Private capability pointer, or null for a non-embedded Host.
 * @throws Nothing.
 * @note The pointer remains valid only while `host` lives.
 */
inline I2Host* as_i2_host(Host& host) noexcept {
  return dynamic_cast<I2Host*>(&host);
}

}  // namespace ps::benchmark
