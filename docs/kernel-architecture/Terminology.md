# Kernel Terminology

This glossary defines the language used by the current kernel implementation.
Terms that exist only in the accepted future direction are defined in
`docs/roadmap/Kernel-Evolution.md` and must not be described as current runtime
objects.

## Product and Runtime Ownership

**`ps::Host`**
The only supported interface for code outside the backend. Embedded frontends,
the CLI, and the IPC adapter exchange copied public values through this seam.
They do not obtain `Kernel`, `GraphRuntime`, or `GraphModel` references.

**Embedded Host adapter**
The in-process implementation of `ps::Host`. It owns a `Kernel` and internal
interaction state while sharing the process-wide operation plugin owner.

**IPC Host adapter**
The installed client-side implementation of `ps::Host`. It translates Host
calls into the versioned local IPC protocol and owns client polling and mapped
image lifetimes, not daemon sessions or backend runtime objects.

**`Kernel`**
The internal multi-graph facade and composition owner for graph, compute,
cache, traversal, inspection, and persistence services.

**`GraphRuntime`**
The per-graph resource container. It owns one `GraphModel`, one bounded
graph-state lane, events, current schedulers, and platform runtime resources.
It is not the owner of compute dependency planning.

**`GraphModel`**
The in-memory graph state: nodes, topology adjacency, parameters, outputs,
cache metadata, timing data, and graph-scoped runtime metadata.

## Graph State and Persistence

**Graph-state operation**
An operation that reads or mutates visible graph state without becoming a
compute task, such as graph document loading, cache commands, inspection, or
ROI projection.

**`GraphStateExecutor`**
The per-graph exclusive access mechanism used by current graph-state operations
and visible compute requests. It owns one worker and a FIFO queue of at most 64
waiting callbacks, excluding the at-most-one active callback. Full-queue
submission blocks; close stops admission, drains prior work, and joins the
worker. Concurrent closers wait for the durable completion generation they
joined, even if failure recovery reopens the lane before they wake. It is
separate from scheduler dispatch and its worker is not a scheduler worker slot.

**Graph document**
The persisted representation used to create or update graph state. YAML is the
current concrete format; the term graph document describes the behavior
without treating a serialization library as graph state.

**Per-graph exclusive access**
The current behavior in which graph-state operations and compute requests do
not concurrently read or mutate the same visible `GraphModel`.

## Compute Planning and Execution

**`ComputeIntent`**
The semantic quality/update intent of a request. `GlobalHighPrecision` and
`RealTimeUpdate` select planning and operation semantics and also select a
per-graph scheduler-map route. The value is not passed as scheduler task
metadata and does not define a thread pool, task priority, QoS, deadline,
fairness, cancellation mode, or commit policy.

**`ComputeService`**
The internal compute facade. It coordinates request validation, planning,
cache policy, dirty work selection, operation resolution, dispatch, metrics,
and output commit through narrower collaborators.

**`FullTaskGraph`**
The complete node/tile task shape for one graph generation, compute intent, and
task-shape configuration. Request target, cache state, and dirty state do not
create this shape.

**`ComputePlan` / `ComputeTaskGraph`**
The request-scoped static plan produced by pruning a full task graph to a target
and dependency cone. It remains immutable while scheduler-visible tasks derived
from it may execute.

**`DirtyRegionSnapshot`**
Graph-scoped ROI and lifecycle state that records dirty sources, affected
regions, tiles, and edge mappings. It is not a compute task graph or scheduler
queue.

**`DirtyUpdateWorkSet`**
The active task subset selected from an existing request plan by a dirty
snapshot. Selection may activate or clip planned tasks but does not create new
node or tile task shapes.

**`DirtyControlLane`**
The serialized path that applies node-originated dirty lifecycle updates and
refreshes graph-scoped dirty state. It does not own compute tasks.

**`ComputeTaskDispatcher`**
The execution orchestrator that owns dependency counters, ready release,
temporary results, and completion aggregation. Its full HP dispatch owns final
result commit; dirty executors reuse its source-first submission helper and own
their staged commit through dirty write buffers. It pushes only concrete ready
task handles or callbacks to the scheduler.

**`ReadyTaskSubmission`**
A conceptual ready submission represented by a borrowed `TaskHandle` or an
owned callback whose compute dependencies are already satisfied. It may carry
scheduler metadata, but never transfers `GraphModel`, a task graph, or dirty
propagation ownership to the scheduler.

## Scheduling, Cache, and Data

**`IScheduler`**
The current scheduler interface. A scheduler instance owns worker lifecycle,
ready queues, batch/epoch state, and completion/exception publication for one
per-graph intent route. Threaded implementations combine policy and physical
resource ownership; `serial_debug` executes synchronously. This is current
behavior, not the accepted long-term resource ownership model.

**Scheduler worker request**
The configured pre-planning value. Zero means automatic and one through eight
are exact; any larger value is invalid. Automatic resolution is
`min(max(1, detected hardware concurrency), 8)`. The request is not yet a
reservation or a count of running threads.

**Resolved worker grant**
The nonzero one-through-eight value produced before scheduler construction. It
is the built-in CPU worker ceiling and the ABI v2 `num_workers` hard ceiling for
one trusted plugin instance. A plugin may own fewer worker threads but must not
own more. It is distinct from the final process slot charge because a
built-in GPU scheduler also charges its potential device worker and built-in
`serial_debug` charges zero.

**Scheduler worker slot**
One conservative admission unit for a potential scheduler-owned physical
worker. A slot may remain reserved when a device is unavailable or a conforming
plugin creates fewer workers. It is not a ready callback, operation-internal
thread, graph-state executor, daemon worker, or observed OS thread.

**`SchedulerWorkerBudget`**
The process-lifetime, mutex-serialized ledger with a fixed 32-slot ceiling
shared by every embedded Host and Kernel. It admits scheduler plans but owns no
threads, queue, policy, or fairness. It is current containment for per-graph
workers, not the target `ExecutionService` or `ResourceLedger` from ADR 0003.

**Scheduler worker reservation**
A move-only RAII owner of admitted slots. Graph load atomically acquires an
HP/RT reservation pair before constructing either scheduler, with one owner
for each intent; replacement acquires one candidate reservation while the old
owner remains live. A `ReservationOwnedScheduler` destroys its concrete
scheduler before returning the reservation exactly once.

**`SchedulerTaskRuntime`**
The scheduler-owned push-only ready-task dispatch mechanism. It accepts initial
and newly released ready batches; it does not pull from a plan, derive tasks,
inspect graph topology, or commit graph state.

**`SchedulerTaskPriority`**
The current independent `Normal` or `High` queue hint. It is orthogonal to
`ComputeIntent`: HP and RT dirty source batches both use `High`, while their
downstream groups use `Normal`.

**Scheduler epoch**
A scheduler-local nonzero batch identity used to reject stale queued work and
ignore stale completion publication. Zero is compatibility work. It is not a
dirty generation, graph revision, Run identity, deadline, or cooperative
cancellation token.

**HP cache**
`cached_output_high_precision`, the only authoritative reusable in-memory
image result stored on a graph node.

**`RealtimeProxyGraph`**
Runtime-owned transient RT output state. It is not authoritative HP cache and
is not persisted as reusable graph cache.

**`ImageBuffer`**
The current image payload contract: two-dimensional extent, channel count,
one scalar type, device, row stride, shared data ownership, and optional
backend context. It is not a general Tensor, Deep Image, or vector-scene model.

**Operation provider**
An implementation source for operation callbacks and metadata. The current
built-in implementation uses OpenCV in private backend code; public operation
contracts use Photospider values.

**Adapter**
A narrow translation at an external library, transport, or product edge. An
adapter converts representations without becoming the owner of graph,
planning, cache, or scheduling semantics.

## Terms That Must Remain Distinct

- `ComputeIntent` is not scheduler priority or commit policy.
- Dirty generation is not scheduler epoch.
- A graph-state operation is not a compute task.
- `DirtyRegionSnapshot` is not `ComputeTaskGraph`.
- A ready task is not a task graph.
- HP cache is not RT proxy state.
- `ImageBuffer` is not the future general data model.
- A worker request is not a resolved grant, and a grant is not necessarily the
  final scheduler slot charge.
- A reserved scheduler worker slot is not proof of one currently running
  thread.
- `SchedulerWorkerBudget` is not a worker pool, fairness authority, or a limit
  on every thread in the process.
- The current per-graph `IScheduler` is not the target process execution
  domain.
