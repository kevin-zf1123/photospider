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

`GraphRuntime` owns this lifecycle ordering for registered schedulers. A
scheduler is attached before it starts; `GraphRuntime::start()` starts
previously registered schedulers, `GraphRuntime::set_scheduler()` starts a new
scheduler only after attach when the runtime is already running, and
`GraphRuntime::stop()` shuts registered schedulers down. `Kernel` bootstrap code
must register schedulers through `GraphRuntime` instead of pre-starting them.
Scheduler shutdown must publish the stop state under the same synchronization
used by idle worker waits and completion waits before notifying those waiters.
This prevents shutdown from missing a worker that is transitioning into
condition-variable sleep after the final task in a batch has completed.

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

In the current `DirectGraphCommit` path, a `RealTimeUpdate` request still
performs both HP and RT dirty work for the same dirty region, but
`IntentUpdateCoordinator` runs the HP sibling before the RT sibling. The RT
path stages proxy writes in `RealtimeDirtyWriteBuffer` and commits them after
RT dirty work drains. Cross-intent HP/RT sibling concurrency is a future
buffered-commit capability, not a scheduler workaround. Schedulers should keep
handling resource dispatch for ready task callbacks, epochs, cancellation, and
queue policy without making RT output a formal HP cache source.

## Current Dispatch State

Parallel compute planning and plan execution belong to `ComputeService`
collaborators: `FullTaskGraphExpander`, `NodeCacheTaskGraphPruner`,
`DirtyRegionPlanner`, `DirtySnapshotTaskGraphPruner`,
`IntentUpdateCoordinator`, and `ComputeTaskDispatcher`. After pruning,
`ComputeTaskDispatcher` materializes either the node/cache-pruned task graph or
the dirty-clipped update work set into concrete tasks and submits ready work
through the configured `IScheduler` instance for the relevant `ComputeIntent`
via `SchedulerTaskRuntime`.

`GraphRuntime` owns graph state, the `GraphStateExecutor`, scheduler
registration, events, and platform resources. It no longer exposes a general
worker queue, task graph, or completion-counter API. Graph-state operations and
compute requests that mutate the visible `GraphModel` use
`GraphStateExecutor`, including scheduler-backed parallel compute. The
scheduler-facing design should extend `IScheduler` and
`SchedulerTaskRuntime`; it should not bypass graph-state access by taking
direct ownership of the runtime model.

## Built-in Schedulers

| Type | Meaning |
| --- | --- |
| `cpu_work_stealing` | Multi-threaded CPU scheduler. |
| `serial_debug` | Deterministic single-threaded debugging scheduler. |
| `gpu_pipeline` | Heterogeneous CPU/GPU scheduler. |
| `heterogeneous` | Alias for `gpu_pipeline`. |

## Plugin Discovery vs Graph Selection

`graph_cli` scans `scheduler_dirs` during startup after config load and before
graph load. That scan only makes plugin-provided scheduler types available to
the scheduler factory. It does not change the scheduler instances already
selected for a graph.

Newly loaded graphs receive scheduler instances from
`Kernel::SchedulerConfig`, which is populated from `scheduler_hp_type`,
`scheduler_rt_type`, and `scheduler_worker_count`. A plugin scheduler therefore
becomes active only when one of those config keys names the scanned plugin type,
or when the user later runs `scheduler set <hp|rt> <type>` for the current
graph. `scheduler plugins` reports discovery state; `scheduler get all` reports
the actual HP/RT scheduler instances attached to the current graph.

## Scheduler Dispatch Boundary

`IScheduler` no longer exposes compute-planning helpers. Removed planning
interfaces must not be reintroduced, so scheduler implementations cannot
accidentally own graph/task planning.

The target model is:

```text
GraphModel topology
  -> FullTaskGraphExpander
  -> FullTaskGraph
  -> NodeCacheTaskGraphPruner
  -> ComputePlan / pruned ComputeTaskGraph
  -> DirtyRegionSnapshot
  -> DirtySnapshotTaskGraphPruner
  -> DirtyUpdateWorkSet
  -> Scheduler resource dispatch
```

Device selection, queue selection, batching, worker reservation, cancellation,
and resource-specific dispatch belong in the scheduler. Dirty propagation,
dirty work-set activation, node/tile expansion, monolithic dirty escalation,
and logical compute-task derivation belong before scheduler dispatch.

The dirty control lane is not a dirty-feature-specific scheduler queue. Dirty
nodes update graph-scoped dirty lifecycle and ROI state through a serialized
control path; the dispatcher materializes dirty work generations from that
state and submits only concrete ready task callbacks to the scheduler.
Scheduler implementations should make decisions from generic ready-task
metadata such as epoch, dirty generation, and optional scheduler-specific
priority hints. A scheduler may drop stale queued work by epoch, use FIFO/LIFO
or work-stealing queues, route CPU/GPU resources, or keep older work running,
but it should not receive task graphs or require a bespoke dirty-source queue.

The compute-service path derives planned work before scheduler dispatch and
does not expose a compatibility compute path outside planned-task dispatch. An
empty planned dispatch is acceptable only when the target already has reusable
HP output; otherwise it is a planning contract error.

## Epoch and Cancellation

Scheduler queues use epochs to cancel stale queued work. Epoch `0` is treated
as non-cancelable compatibility work. New interactive scheduling should assign
real epochs so obsolete RT work can be dropped.

## Observability

`GraphRuntime::SchedulerEvent` records assignment, node execution, and tile
execution events. This is useful for validating scheduler behavior.

## Development Direction

- Keep `IScheduler` as the formal public scheduler interface.
- Keep planned parallel work routed through scheduler-owned task runtimes.
- Keep graph-state commands and visible graph compute requests behind
  `GraphStateExecutor`.
- Keep scheduler runtimes ready-task-only: they receive concrete callbacks with
  epoch/generation metadata and optional scheduler-specific hints, not task
  graphs or dirty work-set state.
- Keep plugin scheduler lifecycle compatible with `Plugin-ABI.md`.
- Continue later work on richer annotated task pools, planner plugin ABI, and
  scheduler policy metadata without moving graph-level dirty planning into a
  scheduler.
