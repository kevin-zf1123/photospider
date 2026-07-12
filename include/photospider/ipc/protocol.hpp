#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/image_buffer.hpp"
#include "photospider/core/result_types.hpp"
#include "photospider/host/compute_request.hpp"

/**
 * @file protocol.hpp
 * @brief Typed public values for Photospider local IPC protocol version 1.
 *
 * The declarations own their data and deliberately exclude JSON parser types,
 * Unix descriptors, socket structures, and backend implementation objects.
 */

namespace ps::ipc {

/**
 * @brief Wire protocol version implemented by this client product.
 *
 * @throws Nothing.
 * @note The constant is compile-time metadata, not a negotiated mutable state.
 */
inline constexpr std::int32_t kProtocolVersion = 1;

/**
 * @brief Largest accepted version 1 JSON payload in bytes.
 *
 * @throws Nothing.
 * @note The four-byte frame header is not included in this payload bound.
 */
inline constexpr std::size_t kMaximumFramePayloadBytes = 16U * 1024U * 1024U;

/**
 * @brief Status-oriented result carrying an owned typed value.
 *
 * @tparam Value Default-constructible public value returned on success.
 * @throws Whatever `Value` or `OperationStatus` throws during value
 *         operations.
 * @note `value` is authoritative only when `status.ok` is true.
 */
template <typename Value>
struct IpcResult {
  /** @brief Completion status for the operation. */
  OperationStatus status;

  /** @brief Owned operation result, or a default value after failure. */
  Value value{};
};

/**
 * @brief Opaque daemon-global identifier for one active graph session.
 *
 * @throws std::bad_alloc when copied or mutated storage cannot be allocated.
 * @note Version 1 values are 32 lowercase hexadecimal characters generated
 *       from operating-system entropy. Clients must not parse or derive them.
 */
struct IpcSessionId {
  /** @brief Opaque identifier copied from a daemon response. */
  std::string value;
};

/**
 * @brief Typed result of `daemon.ping`.
 *
 * @throws std::bad_alloc when copied string storage cannot be allocated.
 * @note The server instance id changes on each daemon process start.
 */
struct DaemonPing {
  /** @brief True when the daemon routed the ping request. */
  bool pong = false;

  /** @brief Opaque identity of the serving daemon process instance. */
  std::string server_instance_id;
};

/**
 * @brief Typed version and capability metadata for one daemon instance.
 *
 * @throws std::bad_alloc when copied strings or methods cannot be allocated.
 * @note `methods` is the exact sorted 55-method version 1 inventory and
 *       advertises only routes admitted by the connected daemon.
 */
struct DaemonVersion {
  /** @brief Negotiated wire protocol version. */
  std::int32_t protocol_version = 0;

  /** @brief Stable daemon service name. */
  std::string service_name;

  /** @brief Photospider project version used to build the daemon. */
  std::string service_version;

  /** @brief Opaque identity of the serving daemon process instance. */
  std::string server_instance_id;

  /** @brief Local transport name; version 1 reports `unix`. */
  std::string transport;

  /** @brief Exact sorted 55-method wire inventory supported by the daemon. */
  std::vector<std::string> methods;
};

/**
 * @brief Public display row for one daemon-owned graph session.
 *
 * @throws std::bad_alloc when copied strings cannot be allocated.
 * @note Only `session_id` addresses later calls; `session_name` is preserved
 *       caller metadata and retains existing Host filesystem semantics.
 */
struct GraphSessionSummary {
  /** @brief Opaque daemon-global session identifier. */
  IpcSessionId session_id;

  /** @brief Caller-provided safe Host session name. */
  std::string session_name;
};

/**
 * @brief Opaque daemon-global identifier for one accepted compute job.
 *
 * @throws std::bad_alloc when copied or mutated storage cannot be allocated.
 * @note Version 1 values are 32 lowercase hexadecimal characters. They remain
 *       valid across direct-client disconnects until release, eviction, or
 *       terminal-record expiry and must never be interpreted as paths.
 */
struct ComputeRequestId {
  /** @brief Opaque compute-job identifier copied from the daemon. */
  std::string value;
};

/**
 * @brief Opaque identity of one protected daemon image artifact.
 *
 * @throws std::bad_alloc when copied or mutated storage cannot be allocated.
 * @note This id names daemon-owned output-store state; it is not a backend
 *       cache key, filesystem path, or image-library handle.
 */
struct OutputArtifactId {
  /** @brief Opaque output identifier copied from validated result metadata. */
  std::string value;
};

/**
 * @brief Opaque identity of one active artifact-delivery lease.
 *
 * @throws std::bad_alloc when copied or mutated storage cannot be allocated.
 * @note The stable id is returned by `compute.result` and may be supplied to
 *       `compute.release` so job ownership and its matching lease are released
 *       atomically. It contains no descriptor or client-memory ownership.
 */
struct DeliveryLeaseId {
  /** @brief Opaque delivery-lease identifier copied from the daemon. */
  std::string value;
};

/**
 * @brief Selects the result retained for one polling compute job.
 *
 * @throws Nothing.
 * @note Status jobs invoke the daemon Host status compute path. Image jobs
 *       invoke the Host image compute path exactly once and may publish one
 *       protected artifact after terminal success.
 */
enum class ComputeResultMode {
  /** @brief Retain only the exact terminal operation status. */
  Status,

  /** @brief Retain an optional protected image artifact after success. */
  Image,
};

/**
 * @brief Forward-only lifecycle state of one daemon polling job.
 *
 * @throws Nothing.
 * @note Values may advance only from Queued to Running and then to Succeeded
 *       or Failed. Version 1 publishes no cancelling/cancelled state.
 */
enum class ComputeJobState {
  /** @brief Accepted work waiting for the daemon's joined worker. */
  Queued,

  /** @brief Work currently executing through the daemon Host boundary. */
  Running,

  /** @brief Immutable terminal work with a successful nested status. */
  Succeeded,

  /** @brief Immutable terminal work with a failed nested status. */
  Failed,
};

/**
 * @brief Owned typed request for `compute.submit`.
 *
 * The request mirrors the public Host compute controls while replacing the
 * embedded Host session label with the daemon's opaque session id and adding
 * the explicit polling result mode.
 *
 * @throws std::bad_alloc when copied string or optional storage allocation
 *         fails.
 * @note The direct Client serializes this value once and never automatically
 *       retries submission after a transport failure. Unknown future wire
 *       members remain a daemon-side compatibility concern.
 */
struct ComputeSubmitRequest {
  /** @brief Opaque daemon session whose graph owns the target node. */
  IpcSessionId session_id;

  /** @brief Nonnegative graph node requested for compute. */
  NodeId node;

  /** @brief Cache precision and persistence controls. */
  HostComputeCacheOptions cache;

  /** @brief Scheduler and quiet-mode controls. */
  HostComputeExecutionOptions execution;

  /** @brief Timing and telemetry controls. */
  HostComputeTelemetryOptions telemetry;

  /** @brief Optional HP or RT compute intent. */
  std::optional<ComputeIntent> intent;

  /** @brief Optional HP-space dirty ROI for intent-aware compute. */
  std::optional<PixelRect> dirty_roi;

  /** @brief Status-only or protected-image result selection. */
  ComputeResultMode result_mode = ComputeResultMode::Status;
};

/**
 * @brief Immutable metadata for one protected tight-row CPU image artifact.
 *
 * @throws std::bad_alloc when copied path or identifier storage cannot be
 *         allocated.
 * @note The value contains no pixel bytes, descriptor, mutable mapping,
 *       backend cache path, or image-library object. Task 4.3 consumes this
 *       metadata while the associated delivery lease protects result-to-open.
 */
struct OutputArtifactMetadata {
  /** @brief Stable daemon artifact identity. */
  OutputArtifactId output_id;

  /** @brief Absolute protected artifact path advertised by the daemon. */
  std::string path;

  /** @brief Image width in pixels. */
  int width = 0;

  /** @brief Image height in pixels. */
  int height = 0;

  /** @brief Number of tightly interleaved channels per pixel. */
  int channels = 0;

  /** @brief Validated channel scalar type. */
  DataType data_type = DataType::FLOAT32;

  /** @brief Materialized memory domain; version 1 requires CPU. */
  Device device = Device::CPU;

  /** @brief Exact tight row width in bytes. */
  std::size_t row_step = 0;

  /** @brief Exact regular-file byte size. */
  std::size_t byte_size = 0;

  /** @brief Filesystem device captured at daemon publication. */
  std::uint64_t filesystem_device = 0;

  /** @brief Filesystem inode captured at daemon publication. */
  std::uint64_t inode = 0;
};

/**
 * @brief Revalidated artifact metadata protected by one stable delivery lease.
 *
 * @throws std::bad_alloc when copied metadata or lease storage cannot be
 *         allocated.
 * @note Repeated successful `compute.result` calls for one retained artifact
 *       return the same lease id while atomically refreshing its server-side
 *       expiry. This value owns only copied metadata, not the lease itself.
 */
struct OutputArtifactDelivery {
  /** @brief Revalidated immutable artifact metadata. */
  OutputArtifactMetadata metadata;

  /** @brief Stable lease identity refreshed by this result operation. */
  DeliveryLeaseId delivery_id;
};

/**
 * @brief Owned non-destructive snapshot of one daemon compute job.
 *
 * @throws std::bad_alloc when copied identifiers, status diagnostics, path,
 *         or optional value storage cannot be allocated.
 * @note `status` is absent for Queued/Running and present for both terminal
 *       states. `output` is present only for a successful terminal nonempty
 *       image result; status polling and submission never publish it.
 */
struct ComputeJobSnapshot {
  /** @brief Opaque identity of the accepted job. */
  ComputeRequestId compute_id;

  /** @brief Opaque session captured at accepted submission. */
  IpcSessionId session_id;

  /** @brief Current forward-only job lifecycle state. */
  ComputeJobState state = ComputeJobState::Queued;

  /** @brief Cancellation capability; version 1 requires false. */
  bool cancellable = false;

  /** @brief Exact immutable terminal status, absent while nonterminal. */
  std::optional<OperationStatus> status;

  /** @brief Optional result-time artifact delivery and lease metadata. */
  std::optional<OutputArtifactDelivery> output;
};

/**
 * @brief Typed acknowledgement returned by `compute.release`.
 *
 * @throws std::bad_alloc when copied identifier storage cannot be allocated.
 * @note A successful acknowledgement always echoes the requested job id and
 *       reports `released=true`. It owns no remaining job or artifact state.
 */
struct ComputeReleaseResult {
  /** @brief Opaque job identity echoed by the daemon. */
  ComputeRequestId compute_id;

  /**
   * @brief True after terminal job ownership and/or its matching orphaned
   *        delivery lease was released.
   */
  bool released = false;
};

}  // namespace ps::ipc
