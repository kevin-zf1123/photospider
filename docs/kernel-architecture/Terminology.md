# Kernel Terminology

This glossary defines the language used by the current kernel implementation.
Terms that exist only in an accepted target decision, including
[ADR 0007](../adr/0007-compute-runs-and-process-execution-have-separate-owners.md),
or the [kernel evolution target](../roadmap/Kernel-Evolution.md) must not be
described as current runtime objects.

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

**`ComputeRun`**
The current private, request-owned execution record for one non-realtime HP
domain or one realtime HP/RT child domain. Its immutable descriptor contains
an opaque non-reused id, session identity, topology-only submission revision,
target, single-domain intent, matching full/interactive quality, and explicit
QoS. It owns monotonic phase and exact-once terminal state plus the full
submission plan/temporary results or dirty HP staging required by its path
through shared control state. Built-in CPU full, dirty, and preflight tasks
retain non-forgeable `ComputeRunLease` values, execute owned callbacks through
the fixed multi-Graph `ExecutionService`, and publish failure only through a
matching `(RunId, RunLocalTaskId)`. Realtime requests currently create paired
child Runs without a `RunGroup`. Authoritative `GraphRevision`, the final
lifecycle registry, and request-owned `RunGroup` remain future work.

**`ComputeRunQos`**
The private immutable scheduling inputs captured by a Run: an explicit
`Interactive` or `Throughput` service class, an optional absolute monotonic
deadline, a positive weight, and an optional positive maximum-parallelism
descriptor. The current service applies class, deadline, and weight to policy
ordering and headroom admission. Maximum parallelism remains recorded but is
not yet an execution cap. A deadline orders interactive work; it does not
expire or cancel a Run. Current Kernel requests use throughput, and none of
these values is inferred from intent or output quality.

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
temporary-result indexing semantics, and completion aggregation. For current
full HP work, the request `ComputeRun` owns the `TaskSubmissionPlan` storage and
temporary result slots while the dispatcher owns their dependency transitions
and final result commit. Dirty executors reuse its source-first submission
helper and own their staged commit through dirty write buffers. Built-in CPU
full, dirty, and preflight paths push lease-backed owned submissions; only
legacy dirty scheduler routes may still push borrowed task handles.

**`ReadyTaskSubmission`**
A move-only service submission whose compute dependencies are already
satisfied. It owns immutable Run/task identity, a matching lease, an
executable, a priority hint, and a trusted host resource declaration. Legacy
schedulers may still receive borrowed handles or owned callbacks, but neither
route receives `GraphModel`, a task graph, or dirty-propagation ownership.

## Scheduling, Cache, and Data

**`IScheduler`**
The current legacy scheduler interface. A serial, GPU, or plugin scheduler
instance owns worker lifecycle, ready queues, batch/epoch state, and
completion/exception publication for one per-graph intent route. Threaded
implementations combine policy and physical resource ownership;
`serial_debug` executes synchronously. Built-in CPU Graph bindings instead use
the fixed Host-composed `ExecutionService` and own no `IScheduler`. The legacy
interface remains current for those routes, but it is not the accepted final
policy-only scheduler generation.

**`SchedulerPolicy`**
The private, stateless comparison seam used by the current service. Its
interactive and throughput implementations rank immutable ready descriptors;
the service-owned store retains Graph/Run fairness state and physical entries.
A policy owns no worker, ready entry, Run, Graph, budget, resource token,
executor, completion route, or lifecycle authority. It is not the future
replacement plugin ABI.

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

**`ResourceVector`**
A complete checked request or snapshot with independent CPU-slot,
retained-memory-byte, scratch-byte, ready-entry, and ready-byte dimensions.
Zero declares no amount in that dimension; it never means an unknown amount
that the ledger may invent.

**`ResourceLedger`**
The private Host-authoritative mint exclusively owned by one
`ExecutionService`. It atomically admits complete vectors, mints only bounded
child grants, and releases exact capacity after parent and child ownership
ends. Default limits belong to the Host composition, not a static process
singleton. It owns no worker, ready ordering, dependency, lifecycle registry,
device/I/O/plugin estimate, or fairness authority.

**Bounded ready store**
The `ExecutionService`-owned policy-aware store whose aggregate entry and
accounted-byte counts cannot exceed immutable ledger limits. It bills one
dispatch as `work_units + ceil(complete_ready_grant_bytes / 4096)`, accumulates
raw Graph service and weight-normalized Run service, honors interactive
deadline ordering, ages ready entries after eight successful dispatches, and
forces throughput progress after at most three consecutive interactive
dispatches while throughput is ready. Initial and dependency-released
submissions cross the same boundary, and Run rows persist across temporary
emptiness. Removing an entry releases its ready grant only after execution
authority is acquired or the entry is purged.

**Resource reservation and grant**
A reservation is the move-only RAII owner of one atomically admitted root
vector; a grant is a move-only, non-forgeable child authority minted within
that vector. Graph load atomically acquires an HP/RT legacy CPU-slot
reservation pair before constructing either scheduler; replacement acquires
one candidate reservation while the old owner remains live. A
`ReservationOwnedScheduler` destroys its concrete scheduler before exact
release. A Run root remains committed until every queued/executing child grant
has ended.

**`SchedulerTaskRuntime`**
The scheduler-owned push-only ready-task dispatch mechanism. It accepts initial
and newly released ready batches; it does not pull from a plan, derive tasks,
inspect graph topology, or commit graph state.

**`SchedulerTaskPriority`**
The current independent `Normal` or `High` ready hint. It is orthogonal to
`ComputeIntent`: HP and RT dirty source batches both use `High`, while their
downstream groups use `Normal`. In the service policy store it is not an
absolute priority: aging can select an older normal-hint entry through a
continuing high-hint stream.

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

**`PixelRect` / `PixelSize`**
External-library-neutral integer geometry values used by public Host and
operation contracts and by private Graph, ROI propagation, dirty-region,
cache-identity, planning, and task state. OpenCV geometry may be constructed
only locally in an OpenCV adapter or provider at the actual matrix/library
call; it is not stored or passed through those kernel contracts.

**Operation provider**
An implementation source for operation callbacks, propagation contracts, and
metadata. Dependency-neutral core operations are always composed at process
seed. The repository OpenCV CPU provider is a separate optional build module
that owns its algorithms, process initialization, and exception translation.
Both it and v2 DSO providers publish into the same provider-neutral registry
slots, so a DSO can replace an active operation and unload restores its
predecessor. Public operation contracts use Photospider values.

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
- `ImageBuffer` is not the
  [target general data model](../roadmap/Kernel-Evolution.md#general-data-and-regions).
- A worker request is not a resolved grant, and a grant is not necessarily the
  final scheduler slot charge.
- A reserved scheduler worker slot is not proof of one currently running
  thread.
- A `ResourceVector` is not a worker pool, observed allocation total, or a
  license to guess undeclared device/I/O/plugin dimensions.
- A root reservation is not an executing callback; a child grant is not
  transferable outside its ledger-created ownership path.
- A legacy per-graph `IScheduler` is neither the current Host-composed built-in
  CPU `ExecutionService` nor the
  [target policy-only scheduler generation](../roadmap/Kernel-Evolution.md#process-execution-domain);
  the current service boundary and remaining lifecycle constraints are fixed
  by
  [ADR 0007](../adr/0007-compute-runs-and-process-execution-have-separate-owners.md).

## Implementation and Validation Entry Points

- `include/photospider/host/host.hpp`
- `include/photospider/core/compute_intent.hpp`
- `include/photospider/core/image_buffer.hpp`
- `include/photospider/scheduler/scheduler.hpp`
- `src/lib/runtime/graph_runtime.hpp`
- `src/lib/graph/graph_model.hpp`
- `src/lib/graph/graph_state_executor.hpp`
- `src/lib/compute/task_graph_planning.hpp`
- `src/lib/compute/dirty_region_snapshot.hpp`
- `src/lib/compute/execution_service.hpp`
- `src/lib/runtime/resource_ledger.hpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_resource_admission.cpp`
- `tests/unit/test_resource_ledger.cpp`
