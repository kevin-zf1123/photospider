# Scheduler Architecture

The kernel has a formal scheduler interface for intent-aware resource dispatch.
This document defines how to read the current implementation after planned
parallel work was routed through scheduler-owned task runtimes.

## Formal Target: IScheduler

`IScheduler` is the formal scheduler interface. A scheduler attaches to a
`GraphRuntime`, starts worker resources, schedules compute, and shuts down
cleanly.

Core lifecycle:

```text
create -> attach(runtime) -> start -> dispatch planned tasks -> shutdown -> detach
```

Compute routes by `ComputeIntent`:

| Intent | Expected scheduler role |
| --- | --- |
| `GlobalHighPrecision` | Throughput-oriented HP compute. |
| `RealTimeUpdate` | Low-latency interactive update. |

`GraphRuntime` stores a scheduler map keyed by `ComputeIntent`.

The long-term scheduler responsibility is resource dispatch after planning.
Schedulers should pull planned or annotated tasks from intent-aware task pools,
choose queue order, batching, worker policy, cancellation, and concrete
execution resources, then dispatch work. They should not own graph-level
dirty-region propagation or compute-task derivation.

In the current parallel runtime path, a `RealTimeUpdate` request can deliberately
submit both RT preview work and HP update work for the same dirty region. The RT
work preserves interactive feedback, while the HP work keeps the mission pool
and formal HP cache synchronized with the graph state. If this dual-submit path
creates blocking, starvation, or worker re-entrancy concerns, those concerns
belong in scheduler design: reserve or steal workers appropriately, use epochs
and cancellation for stale RT work, and avoid waiting policies that can deadlock
worker-owned execution. Do not solve this by making RT output a formal HP cache
source.

## Current Dispatch State

Parallel compute planning and plan execution still belong to `ComputeService`
collaborators: `DirtyRegionPlanner`, `ComputeTaskPlanner`,
`IntentUpdateCoordinator`, and `ComputePlanExecutor`. After planning,
`ComputePlanExecutor` materializes the planned task graph into concrete tasks
and submits ready work through the configured `IScheduler` instance for the
relevant `ComputeIntent` via `SchedulerTaskRuntime`.

`GraphRuntime` still owns graph state, scheduler registration, events, and some
runtime queue APIs used by graph-runtime support paths and tests. Those queues
are no longer the compute-service parallel dispatch route. New
scheduler-facing design should extend `IScheduler` and
`SchedulerTaskRuntime`, not add new dependencies on `GraphRuntime` internal
queues.

## Built-in Schedulers

| Type | Meaning |
| --- | --- |
| `cpu_work_stealing` | Multi-threaded CPU scheduler. |
| `serial_debug` | Deterministic single-threaded debugging scheduler. |
| `gpu_pipeline` | Heterogeneous CPU/GPU scheduler. |
| `heterogeneous` | Alias for `gpu_pipeline`. |

## Scheduler Dispatch Boundary

`IScheduler` no longer exposes compute-planning helpers. Removed planning
interfaces must not be reintroduced, so scheduler implementations cannot
accidentally own graph/task planning.

The target model is:

```text
DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> ComputeTaskPlanner
  -> intent-aware task pools
  -> Scheduler resource dispatch
```

Device selection, queue selection, batching, worker reservation, cancellation,
and resource-specific dispatch belong in the scheduler. Dirty propagation,
node/tile expansion, monolithic dirty escalation, and logical compute-task
derivation belong before scheduler dispatch.

The compute-service path derives planned work before scheduler dispatch and
does not expose a compatibility compute path outside planned-task dispatch.

## Epoch and Cancellation

Runtime and scheduler queues use epochs to cancel stale queued work. Epoch `0`
is treated as non-cancelable compatibility work. New interactive scheduling
should assign real epochs so obsolete RT work can be dropped.

## Observability

`GraphRuntime::SchedulerEvent` records assignment, node execution, and tile
execution events. This is useful for validating scheduler behavior.

## Development Direction

- Keep `IScheduler` as the formal public scheduler interface.
- Keep planned parallel work routed through scheduler-owned task runtimes.
- Avoid adding new permanent dependencies on `GraphRuntime` internal queues.
- Keep plugin scheduler lifecycle compatible with `Plugin-ABI.md`.
- Continue later work on richer annotated task pools, planner plugin ABI, and
  scheduler policy metadata without moving graph-level dirty planning into a
  scheduler.
