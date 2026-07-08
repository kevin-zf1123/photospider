#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "photospider/core/compute_intent.hpp"
#include "photospider/core/inspection_types.hpp"

/**
 * @file event_stream.hpp
 * @brief Public Host event, timing, and scheduler trace snapshots.
 *
 * These snapshot values are copied from backend event buffers and scheduler
 * traces. They do not expose backend event services, scheduler event objects,
 * scheduler instances, task runtimes, or mutable scheduler queues.
 */

namespace ps {

/**
 * @brief Copied frontend compute progress event.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note Events are drained from backend buffers; receiving one event does not
 *       imply the related cache or graph state can be mutated by the caller.
 */
struct ComputeEventSnapshot {
  /** @brief Node that completed a compute step. */
  NodeId node;

  /** @brief Human-readable node name. */
  std::string name;

  /** @brief Backend source label for the event. */
  std::string source;

  /** @brief Elapsed milliseconds reported for the event. */
  double elapsed_ms = 0.0;
};

/**
 * @brief Copied frontend timing row for one node.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note The row is observational telemetry from the most recent timed compute
 *       request and is not scheduler synchronization state.
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
 * @brief Public label for one scheduler trace action.
 *
 * @throws Nothing.
 * @note Values are copied from backend trace actions and deliberately do not
 *       expose scheduler queue internals or task handles.
 */
enum class HostSchedulerTraceAction {
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

  /** @brief Stale dirty generation skipped by the scheduler path. */
  SkipStaleGeneration,

  /** @brief Scheduler path rethrew a captured exception. */
  RethrowException,

  /** @brief Backend action was not recognized by this Host version. */
  Unknown,
};

/**
 * @brief Copied scheduler trace event for frontend inspection.
 *
 * @throws Nothing for value operations.
 * @note `timestamp_us` is the backend clock timestamp serialized as
 *       microseconds since that clock's epoch. It is useful for ordering within
 *       one run, not for wall-clock display.
 */
struct SchedulerTraceEventSnapshot {
  /** @brief Scheduler epoch associated with the event. */
  uint64_t epoch = 0;

  /** @brief Node involved in the scheduler event. */
  NodeId node;

  /** @brief Worker id recorded by the scheduler path, or -1 when unknown. */
  int worker_id = -1;

  /** @brief Public trace action label. */
  HostSchedulerTraceAction action = HostSchedulerTraceAction::Unknown;

  /** @brief Backend high-resolution clock timestamp in microseconds. */
  uint64_t timestamp_us = 0;
};

/**
 * @brief Copied scheduler implementation information.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note The stats string remains backend-defined text for the first Host slice;
 *       callers must not parse it for control flow.
 */
struct SchedulerInfoSnapshot {
  /** @brief Compute intent served by the scheduler. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;

  /** @brief Human-readable scheduler implementation name. */
  std::string scheduler_name;

  /** @brief Backend-defined scheduler statistics text. */
  std::string stats;
};

}  // namespace ps
