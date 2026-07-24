#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "photospider/core/compute_intent.hpp"
#include "photospider/core/inspection_types.hpp"

/**
 * @file event_stream.hpp
 * @brief Public Host event, timing, and execution trace snapshots.
 *
 * These snapshot values are copied from backend event buffers and the private
 * execution domain. They expose no backend event service, physical executor,
 * task runtime, ready store, worker, resource grant, or mutable queue.
 */

namespace ps {

/** @brief Minimum accepted compute-event drain page size. */
inline constexpr std::size_t kComputeEventDrainMinLimit = 1;

/** @brief Maximum accepted compute-event drain page size. */
inline constexpr std::size_t kComputeEventDrainMaxLimit = 1024;

/** @brief Production capacity of each graph's compute-event ring. */
inline constexpr std::size_t kComputeEventRingCapacity = 8192;

/** @brief Maximum canonical UTF-8 byte length of an event name or source. */
inline constexpr std::size_t kComputeEventTextMaxBytes = 1024;

/** @brief Minimum accepted execution-trace page size. */
inline constexpr std::size_t kExecutionTraceMinLimit = 1;

/** @brief Maximum accepted execution-trace page size. */
inline constexpr std::size_t kExecutionTraceMaxLimit = 4096;

/** @brief Production capacity of each graph's execution-trace ring. */
inline constexpr std::size_t kExecutionTraceRingCapacity = 65536;

/**
 * @brief Sentinel indicating that an observation sequence is exhausted.
 *
 * @note Valid compute-event and execution-trace publication sequences are one
 *       through `kObservationSequenceExhausted - 1`; the sentinel is never
 *       assigned to an event.
 */
inline constexpr uint64_t kObservationSequenceExhausted = UINT64_MAX;

/**
 * @brief Copied frontend compute progress event.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note Events are drained from backend buffers; receiving one event does not
 *       imply the related cache or graph state can be mutated by the caller.
 */
struct ComputeEventSnapshot {
  /** @brief Per-session publication sequence; never the exhausted sentinel. */
  uint64_t sequence = 0;

  /** @brief Node that completed a compute step. */
  NodeId node;

  /** @brief Canonical UTF-8 node name retained within the 1,024-byte bound. */
  std::string name;

  /** @brief Canonical UTF-8 source label within the 1,024-byte bound. */
  std::string source;

  /** @brief Elapsed milliseconds reported for the event. */
  double elapsed_ms = 0.0;
};

/**
 * @brief Bounded destructive page of compute progress events.
 *
 * @throws Nothing for scalar access; vector/string mutation can throw
 *         `std::bad_alloc`.
 * @note `events` contains at most `kComputeEventDrainMaxLimit` oldest retained
 *       publications. A successful drain removes only these events and resets
 *       the shared drop count reported by `dropped_count`.
 */
struct ComputeEventBatch {
  /** @brief Oldest retained events removed by this successful drain. */
  std::vector<ComputeEventSnapshot> events;

  /**
   * @brief Sequence after the last returned event, the next publication
   * sequence for an empty page, or the exhausted sentinel.
   */
  uint64_t next_sequence = 1;

  /** @brief Whether retained events remain immediately after removal. */
  bool has_more = false;

  /** @brief Saturating count of drops since the previous successful drain. */
  uint64_t dropped_count = 0;
};

/**
 * @brief Copied frontend timing row for one node.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note The row is observational telemetry from the most recent timed compute
 *       request and is not execution synchronization state.
 */
struct NodeTimingSnapshot {
  /** @brief Node represented by the timing row. */
  NodeId node;

  /** @brief Human-readable node name. */
  std::string name;

  /** @brief Elapsed milliseconds reported for the node. */
  double elapsed_ms = 0.0;

  /** @brief Backend source label for the timing row. */
  std::string source;
};

/**
 * @brief Copied timing snapshot for a graph session.
 *
 * @throws Nothing for value operations except vector/string allocation.
 * @note The total is produced by backend timing collection and should be read
 *       together with the per-node rows captured in the same snapshot.
 */
struct TimingSnapshot {
  /** @brief Node timing rows captured for the graph. */
  std::vector<NodeTimingSnapshot> node_timings;

  /** @brief Backend-reported total elapsed milliseconds. */
  double total_ms = 0.0;
};

/**
 * @brief Public label for one private execution trace action.
 *
 * @throws Nothing.
 * @note Values are copied from the private execution domain and deliberately
 * expose no ready-store entry, resource grant, worker owner, or callback.
 */
enum class HostExecutionTraceAction {
  /** @brief Initial node or task assignment. */
  AssignInitial,

  /** @brief Monolithic node execution. */
  Execute,

  /** @brief Tiled node execution. */
  ExecuteTile,

  /** @brief Dirty source execution. */
  ExecuteDirtySource,

  /** @brief Dirty downstream node execution. */
  ExecuteDirtyDownstreamNode,

  /** @brief Dirty downstream tile execution. */
  ExecuteDirtyDownstreamTile,

  /** @brief Stale dirty generation skipped by the execution path. */
  SkipStaleGeneration,

  /** @brief Execution path rethrew a captured exception. */
  RethrowException,

  /** @brief Backend action was not recognized by this Host version. */
  Unknown,
};

/**
 * @brief Copied execution trace event for frontend inspection.
 *
 * @throws Nothing for value operations.
 * @note `timestamp_us` is the backend clock timestamp serialized as
 *       microseconds since that clock's epoch. It is useful for ordering within
 *       one run, not for wall-clock display.
 */
struct ExecutionTraceEventSnapshot {
  /** @brief Per-session publication sequence; never the exhausted sentinel. */
  uint64_t sequence = 0;

  /** @brief Private execution epoch associated with the event. */
  uint64_t epoch = 0;

  /**
   * @brief Node involved in the execution event.
   *
   * @note A nonnegative value identifies a specific node. `NodeId{-1}` means
   *       that no specific node applies to this event; values below -1 are
   *       invalid.
   */
  NodeId node;

  /**
   * @brief Worker involved in the execution event.
   *
   * @note A nonnegative value identifies a specific worker. `-1` means that no
   *       specific worker applies to this event; values below -1 are invalid.
   */
  int worker_id = -1;

  /** @brief Public trace action label. */
  HostExecutionTraceAction action = HostExecutionTraceAction::Unknown;

  /** @brief Backend high-resolution clock timestamp in microseconds. */
  uint64_t timestamp_us = 0;
};

/**
 * @brief Bounded non-destructive execution-trace sequence page.
 *
 * @throws Nothing for scalar access; vector allocation can throw
 *         `std::bad_alloc`.
 * @note Each event has a sequence greater than the request cursor. Repeating a
 *       request does not remove trace entries. `dropped_count` reports the
 *       exact retained-history and exhausted-publication gap after that
 *       cursor, using saturating arithmetic.
 */
struct ExecutionTracePage {
  /** @brief Retained trace events selected at one locked observation point. */
  std::vector<ExecutionTraceEventSnapshot> events;

  /**
   * @brief Last returned sequence, the input cursor for a pre-exhaustion empty
   * page, or the exhausted sentinel after terminal observation.
   */
  uint64_t next_sequence = 0;

  /** @brief Whether another retained event follows this page. */
  bool has_more = false;

  /** @brief Saturating exact gap after the requested cursor. */
  uint64_t dropped_count = 0;
};

/**
 * @brief Copied private execution-route information.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note The stats string remains backend-defined text for the first Host slice;
 *       callers must not parse it for control flow.
 */
struct ExecutionInfoSnapshot {
  /** @brief Compute intent served by the private route. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;

  /** @brief Exact route name: `cpu`, `gpu_pipeline`, or `serial_debug`. */
  std::string execution_type;

  /** @brief Backend-defined copied execution statistics text. */
  std::string stats;
};

}  // namespace ps
