# Kernel Compute Flow

This document describes current compute request, planning, execution, HP/RT,
commit, event, and error behavior. Module ownership is summarized in
`Compute-Boundaries.md`.

## Entry Points

Typical frontend flow:

```text
CLI / TUI
  -> ps::Host
  -> embedded Host adapter
  -> InteractionService
  -> Kernel
  -> GraphRuntime
  -> ComputeService
  -> OpRegistry / GraphCacheService / GraphTraversalService
  -> RoiPropagationService / GraphExtentResolver
```

`Kernel` owns the multi-graph API. `GraphRuntime` owns one graph model, the
per-graph `GraphStateExecutor`, event service, platform context, and scheduler
instances. The embedded composition root also creates one private CPU
`ExecutionService` before Kernel; Kernel injects that owner into each
request-local `ComputeService`.

`ps::Host` is the public frontend-facing interface. The embedded Host adapter
copies public request/result values and uses the internal `InteractionService`
wrapper and `Kernel`; CLI/TUI code does not include or call those backend
facades directly. In the dirty-region context, Host exposes graph-scoped dirty
snapshot and lifecycle values, while `InteractionService` remains an internal
translation boundary rather than the authoritative source of dirty-region
generation or propagation.

Frontend compute commands build a public `ps::HostComputeRequest` rather than
passing positional boolean flags or internal request types through the public
seam. The embedded Host adapter translates that value to
`Kernel::ComputeRequest`. `Kernel` owns graph lookup, runtime start, quiet-mode
and skip-save side effects, async scheduling, image extraction, and LastError
mapping. It then translates the internal request to
`ComputeService::Request`, which carries node target, cache, telemetry, intent,
dirty ROI, session identity, and explicit Run QoS data. Parallel/runtime
selection is carried separately as `ComputeService::ExecutionStrategy`. The
added identity and QoS values remain private descriptor inputs and do not
change the public Host request or plugin ABI.

The dirty ROI remains a kernel-owned `PixelRect` while it is copied from
`HostComputeRequest` through `Kernel::ComputeRequest`, graph propagation,
planning, task selection, staged execution, and `NodeExecutor`. Extents use
`PixelSize`. No OpenCV geometry conversion occurs on this path; a provider may
create a local OpenCV rectangle or size only at an actual matrix or algorithm
call.

The CLI/REPL frontend is a permanent batch-oriented surface. It does not expose
RT intent commands, dirty ROI creation, or dirty source lifecycle commands such
as `compute rt`, `--dirty-roi`, `dirty begin`, `dirty update`, or `dirty end`.
`RealTimeUpdate` and dirty source lifecycle APIs are available through Host and
kernel/test callers, but the CLI does not expose them and must not be treated as
a production realtime control surface.

Compute does not acquire scheduler capacity per request. Before a graph can be
published, load resolves its configured worker request (`0` becomes
`min(max(1, hardware_concurrency()), 8)`, explicit `1..8` remains exact), plans
the HP and RT scheduler charges, and atomically reserves their combined demand
from the process-wide 32-slot ledger. The accepted pair contains one move-only
reservation for each scheduler owner, retained across every synchronous or
asynchronous compute request. Only built-in `serial_debug` is a zero-slot
scheduler; built-in CPU and ABI v2 plugin schedulers charge the resolved grant,
while the built-in GPU/heterogeneous scheduler also charges its potential
device worker.
This admission step bounds current Graph-owned scheduler workers, not callback
count or all threads used by compute and its operations. In particular, issue
#68's injected CPU service pool is not charged by this transitional ledger;
the still-allocated per-Graph CPU scheduler retains the existing charge until
issue #69 removes duplicate owners.

`GraphTraversalService` is topology-only. It provides traversal order and
explicit upstream/downstream topology queries from `GraphModel` adjacency.
Dirty-region demand and ROI projection use `RoiPropagationService`, while
formal propagation extents come from `GraphExtentResolver`.

The current compute planning flow separates request-scoped static planning from
per-update dirty work selection:

```text
ComputeService facade
  -> GraphModel topology / GraphTraversalService queries
  -> FullTaskGraphExpander
  -> FullTaskGraph
  -> NodeCacheTaskGraphPruner
  -> ComputePlan / pruned ComputeTaskGraph
  -> DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> DirtySnapshotTaskGraphPruner
  -> DirtyUpdateWorkSet
  -> Run-owned TaskSubmissionPlan / ComputeTaskDispatcher
  -> ready-task ExecutionService or legacy scheduler dispatch
```

Before any of those planning steps, each non-realtime HP service call creates
exactly one request-owned `ComputeRun`. It captures a fresh opaque id, session
identity, target, `GlobalHighPrecision` intent, full quality, explicit QoS, and
the current topology generation as a submission revision. That topology value
is provenance only, not the authoritative graph-wide `GraphRevision` or a
commit predicate. Sequential, scheduler-backed, and explicit dirty HP variants
share this boundary. On the scheduler-backed full HP path, shared Run control
owns the materialized plan and runner; every real ready task retains a
non-forgeable Run lease and `(RunId, RunLocalTaskId)` identity through
execution, dependency release, validation, and commit. Built-in CPU full HP
packages each dependency-ready task as a move-only `ReadyTaskSubmission` and
uses the injected service's single-Run CPU batch. Serial, GPU, and plugin full
HP retain the lease-backed callback path. Explicit dirty HP keeps its separate
synchronous borrowed-handle path. `RealTimeUpdate` creates no mixed Run or child
Runs; paired Run/`RunGroup` settlement remains future work.

`FullTaskGraphExpander` expands the raw graph into the full node/tile task graph
for one compute domain. It does not depend on the request target, cache state,
or dirty snapshot. `NodeCacheTaskGraphPruner` then prunes that graph to the
request target/dependency cone and records cache availability for selected
nodes. Dirty updates add a separate `DirtySnapshotTaskGraphPruner` pass that
annotates the selected graph with dirty metadata and produces the active
`DirtyUpdateWorkSet`.

Single-threaded and parallel execution use the same full-expansion and
node/cache-pruning semantics. Their execution mechanisms differ, while graph
traversal, dirty planning, and task-graph pruning remain compute-owned.

`ComputePlan` is a static analysis for the current compute request and domain.
It is derived while graph state is stable and remains the topology contract for
that request, whether the current path writes directly to visible graph state or
uses the existing dirty-path staged buffers. Dirty updates do
not rebuild topology semantics. They use the current `DirtyRegionSnapshot` and
dirty ROI to activate or clip a `DirtyUpdateWorkSet` from the plan for each HP
or RT update queue.

The request plan must enumerate the real compute tasks available to the
request, including tile tasks when the selected implementation is tiled. Dirty
state only prunes or activates tasks from that enumerated graph. It must not
expand new tile tasks during dirty clipping; this keeps full-frame tiled
parallelism and dirty ROI execution on the same task model.

The request's `ComputeTaskGraph` is immutable while scheduler tasks derived
from it may still run. Each dirty compute request creates a generation-local
selection overlay from its plan and snapshot, then pushes only ready handles or
callbacks. The scheduler does not receive a replaceable task-graph object.

Execution granularity is a separate layer. A selected operation implementation
is node-wide, monolithic, or tiled. The current full-task population maps
`MICRO` metadata to 16-pixel tasks, `MACRO` metadata to 256-pixel tasks, and an
unspecified preference to 128-pixel tasks. Edge tasks are clipped to the output
extent.

Dirty snapshot grids are different metadata: HP snapshots materialize Micro
keys on a 64-pixel HP grid and RT snapshots materialize Micro keys on a
16-pixel proxy grid. The dirty selector intersects those records with task
shapes already present in the full graph. It does not create `ReTileTask`, RT
Macro_64 records, or dynamic Micro/Macro conversion. HP and RT plans remain
single-domain; realtime intent coordinates separate sibling work without
creating cross-domain task dependencies.

## Compute Intents

The kernel recognizes two formal compute intents:

| Intent | Meaning |
| --- | --- |
| `GlobalHighPrecision` | Full-quality HP compute. Owns high-precision output. Non-realtime compute enables only this HP path. |
| `RealTimeUpdate` | Interactive realtime update. Requires a dirty ROI and enables the HP/RT dual path. |

The intent model is formal. `ComputeService` remains the compute facade and
planning boundary. Private scheduler-route metadata pairs the configured
scheduler owner with its physical path: built-in CPU non-dirty full HP uses the
injected service, while other full-HP, dirty, and realtime routes use the
configured per-Graph `IScheduler` task runtime.

HP/RT dual path semantics belong to realtime intent, not to the parallel
execution mode. In realtime mode, HP computes the full-size authoritative node
work while RT computes the downscaled proxy, currently one quarter of width and
height, or one sixteenth of the pixel count. `IntentUpdateCoordinator` launches
two sibling calls, and each sibling looks up the per-graph scheduler registered
for its intent route. `ComputeIntent` does not itself specify QoS or physical
priority.

The current collaborator responsibilities and non-ownership boundaries are
documented in `Compute-Boundaries.md`.

## Sequential Compute

Sequential compute uses recursive dependency resolution:

1. Validate the target node.
2. Resolve traversal order and optionally clear caches.
3. For each dependency, compute upstream nodes recursively.
4. Copy the static `ParameterMap` into `runtime_parameters` and overlay
   connected named `ParameterValue` outputs without format conversion.
5. Resolve an operation implementation for HP intent.
6. Execute monolithic or tiled operation.
7. Store output, emit events, update timing, and save disk cache when enabled.

Sequential compute is useful for simple execution and debugging. It creates
the same internal full-expansion and node/cache-pruned task graph semantics as
the parallel path before executing the recursive path.

## Parallel Compute

Parallel compute derives a `ComputePlan` by expanding the full task graph and
then pruning it with `NodeCacheTaskGraphPruner` from `topo_postorder_from`.
`ComputeDispatchPlanBuilder` records that cache-pruned plan for inspection.
The request `ComputeRun` owns the `TaskSubmissionPlan` that materializes the
plan's `ComputeTaskGraph` into dependency counters, ready values, operation
variants, and temporary result slots. For a built-in CPU route the dispatcher
creates immutable, lease-backed `ReadyTaskSubmission` values and submits one
initial ready batch to the injected `ExecutionService`; dependent completion
creates further submissions through the same active Run. Legacy full-HP routes
retain owned scheduler callbacks. Tiled operations may spawn micro-tasks and
retire the selected runtime's logical completion count.

The selected transitional scheduler has already been admitted for its full
lifetime before ready work is submitted. A running compute therefore consumes
no new ledger slots. The current service likewise performs no ledger
admission; it serializes one Run and reconfigures its concrete CPU runtime to
the exact trusted built-in grant. Scheduler inspection and replacement share
the per-graph `GraphStateExecutor` boundary with compute; replacement cannot
overlap a compute callback sequence, and it publishes scheduler ownership plus
private route metadata in one transaction. Failed candidate planning, attach,
or start leaves the old scheduler, route, and compute behavior unchanged and
returns only candidate capacity.

`ComputeTaskDispatcher` keeps plan execution, dependency accounting, sparse
node-id mapping, temporary-result indexing, event logging, exception
propagation, and final target selection inside the compute-service boundary.
The Run owns the corresponding plan and result-slot storage. The dispatcher
uses scheduler task-runtime queues for already-planned work; it does not make
the scheduler own dirty propagation, compute-task derivation, or the task graph
itself. If the pruned planned dispatch is empty while the target has no reusable
HP output, the dispatcher reports a planning contract error instead of falling
back to recursive sequential compute.

For dirty execution, `DirtySnapshotTaskGraphPruner` materializes only the active
`DirtyUpdateWorkSet` selected from the request's `ComputeTaskGraph`. Runtime
dependency counters and ready-task queues are execution artifacts; they are not
stored in `DirtyRegionSnapshot` and are not owned by the scheduler.

Explicit Host begin/update/end calls identify a dirty source node and update
graph-scoped lifecycle facts; they do not trigger compute. The current backend
has no node subscription, automatic request launch, or active-request
coalescing contract.

Lifecycle updates enter the per-graph serialized path and call
`DirtyControlLane`. The lane updates source state, rebuilds source-local derived
records for the event domain, and returns wakeup/cutoff hints in
`DirtyControlLaneResult`. This lifecycle path does not traverse downstream
edges, and the hints currently have no production compute consumer. The
embedded Host exposes copied inspection values, not internal control fields.
Schedulers receive only pushed ready handles or callbacks with their own batch
epoch and priority hint; dirty generation is not forwarded as the scheduler
epoch.

## Graph-State Access and Commit Policy

Graph-state operations such as YAML loading, cache commands, inspection, and
ROI projection are operations on the visible `GraphModel`. They are not
compute-task dispatch and are not routed through `SchedulerTaskRuntime`.

The current default is per-graph exclusive access through
`GraphStateExecutor`: graph-state operations and compute requests for the same
graph do not concurrently read or mutate the visible `GraphModel`. This includes
scheduler-backed parallel compute; the outer Kernel request enters
`GraphStateExecutor`, while ready node/tile callbacks are dispatched through
the scheduler runtime inside that boundary. This keeps graph topology, cache
fields, dirty snapshots, timing, and node runtime state coherent without
routing non-compute commands through scheduler queues.

Scheduler and required-session lifetime are coordinated with this boundary.
The graph-state portions of synchronous and asynchronous compute, scheduler
information, scheduler replacement, required graph save, node-YAML replacement,
and ROI projection are serialized by the per-graph executor. Required-session
lifetime admission also covers timing inspection and all-cache clearing through
public result/status translation, even though those calls do not introduce a
new scheduler task boundary. During embedded close, the Host first publishes
its close marker and lets synchronous calls admitted before that marker finish
public translation; graph-state users finish submitting while the lane remains
accepting.
Kernel then stops lane admission before the Host waits for async submission
placeholders and status publication. This wakes a producer blocked by the full
FIFO without requiring queue space; previously admitted callbacks still drain
before Kernel joins the executor and invokes runtime stop. Runtime startup may
occur before graph-state submission; the embedded Host admits the complete call
against close so close cannot erase the runtime during startup or before
graph-state completion. Node replacement and ROI projection also perform
required-node lookup and the operation in one work item, preventing a
clear/reload check-then-act gap. Scheduler information copies name/statistics
before leaving the boundary; no raw scheduler pointer survives it.

The current dirty update implementation uses staged output commits for HP/RT
sibling safety. A standalone `GlobalHighPrecision` dirty request stores its
`HighPrecisionDirtyWriteBuffer` in the request Run. A `RealTimeUpdate` HP
sibling still keeps that buffer callback-local and commits to the
visible `GraphModel` only after the RT sibling has committed. RT dirty workers
write `RealtimeProxyWriteBuffer` and commit to the runtime-owned
`RealtimeProxyGraph`. There is no general graph-revision or interruptible
commit policy in the current implementation. ADR 0003 and the kernel evolution
roadmap define that accepted direction separately from current behavior; ADR
0007 fixes the complete Run/revision/commit race, of which the bounded HP
Run/lease/completion-isolation slice is current. Commit policy remains
conceptually separate
from `ComputeIntent`, because HP/RT intent semantics do not define visibility
or interruption.

## GlobalHighPrecision

`GlobalHighPrecision` is the full-quality path. Without a dirty ROI it performs
normal full compute. With a dirty ROI it enters the HP dirty update path; it
does not unconditionally replace that request with full-frame recompute.

HP dirty-region update is a first-class dirty-ROI consumer, not just a full
recompute fallback. Normal dirty ROI requests compute a backward ROI plan, align
dirty regions to HP tile boundaries, clip the HP work set from the request's
`ComputeTaskGraph`, update affected HP tiles, record HP ROI/version metadata,
and can schedule downsample work to refresh `RealtimeProxyGraph` state.
`IntentUpdateCoordinator` routes global HP dirty requests to this path and
records `intent_coordinator_global_dirty_update`.

Forced HP dirty updates are the exception: when `force_recache=true`, the HP
staging buffer intentionally does not seed pixels from the previous HP cache, so
the executor expands the HP planning ROI to the target node's full current HP
extent before committing. This preserves complete authoritative HP output while
keeping non-forced dirty ROI requests local.

Dirty-region state planning runs through the graph-scoped
`DirtyRegionPlanner`, and the resulting `DirtyRegionSnapshot` feeds dirty
work-set materialization and interaction-facing inspection summaries.

Consequently, one public Host HP dirty request exercises one continuous
kernel-native geometry path: request validation, graph-scoped backward
projection, immutable plan selection, source-first ready dispatch, node
execution, and staged HP commit all observe `PixelRect`/`PixelSize` values.

## RealTimeUpdate

`RealTimeUpdate` requires a dirty ROI. A request without `dirty_roi` is invalid
and returns a clear public `ps::Host` status/error value. The embedded adapter
derives that value from internal Kernel and InteractionService diagnostics. The
request does not implicitly mean full-frame RT update.

With a valid dirty ROI, realtime compute enables both paths. RT is launched
first and updates a low-resolution `RealtimeProxyGraph`; HP updates the
full-size authoritative output for the affected graph work through a staged
buffer. If the request is forced, the HP sibling follows the same full-frame HP
planning rule as Global HP dirty update, while RT proxy work remains scoped to
the RT dirty plan. When HP and RT scheduler runtimes are available,
`IntentUpdateCoordinator` starts both siblings concurrently, waits for RT first,
and uses a sibling commit gate so HP mutates `GraphModel` only after RT proxy
commit succeeds. Without scheduler runtimes, the same callbacks run inline in
RT-then-HP order.

The concurrent path also shares one request-owned per-node synchronization
object between the siblings. It protects live `Node` snapshot and
format-neutral parameter resolution for the same node without merging the two
domain plans:
different nodes and the operation bodies remain concurrent, and the object is
released after both sibling futures have drained, including failure cleanup.

Realtime planning is intentionally per path, not a single mixed-domain planner
call. `IntentUpdateCoordinator` dispatches sibling HP and RT update callbacks
and records RT-first/concurrent stages for Dirty RT requests. Each path uses a
single-domain request plan and a same-domain dirty snapshot: the HP callback
uses a `GlobalHighPrecision` node/cache-pruned plan with an HP dirty snapshot,
and the RT callback uses a `RealTimeUpdate` node/cache-pruned plan with an RT
dirty snapshot. HP dirty node execution writes into
`HighPrecisionDirtyWriteBuffer`; RT dirty node execution writes into
`RealtimeProxyWriteBuffer` and commits only to `RealtimeProxyGraph`. The dirty
snapshot clips or activates the update work set from the path's task graph.
This keeps full task expansion, node/cache pruning, dirty snapshot pruning, and
output commit as separate contracts for each compute domain.

The passed dirty ROI is converted into graph-scoped planner state for the
current request. Public `ps::Host` begin/update/end methods translate through the
embedded adapter to internal `Kernel` / `InteractionService` dirty-source
lifecycle methods, so frontend or node-facing code writes state through the same
graph-owned boundary. The Host lifecycle call is currently the public write
surface; there is no public node-event subscription that automatically creates
or schedules a compute request.

RT task graph expansion is domain-aware. When an operation has distinct HP and
RT metadata, the `RealTimeUpdate` plan uses RT metadata for tile size and
dependency ROI planning, while the HP sibling uses HP metadata. This keeps RT
Micro_16 planning independent from HP Macro_256 throughput defaults.

The current implementation has no node-to-backend realtime event subscription.
Nodes and Host lifecycle calls may update graph-owned dirty state, while
`InteractionService` remains only an internal translation boundary and the CLI
remains batch-oriented. Any subscription surface is outside the current
software contract.

Current defaults:

| Parameter | Current value | Status |
| --- | --- | --- |
| RT downscale factor | `4` | Tunable implementation default. |
| RT micro tile size | `16` | Tunable implementation default. |
| RT macro tile size | `64` | Tunable implementation default; same numeric size as HP micro, different domain. |
| HP micro tile size | `64` | Tunable implementation default. |
| HP macro tile size | `256` | Tunable implementation default. |

These constants are not permanent ABI.

## Events and Timing

`GraphEventService` publishes per-node compute events into a thread-safe,
fixed-capacity ring. The production capacity is 8,192 events per graph. Each
accepted publication receives a monotonically increasing unsigned 64-bit
sequence in `1..UINT64_MAX-1`; `UINT64_MAX` is an exhaustion sentinel and is
never assigned to an event. The ring evicts exactly its oldest retained event
when full and accounts for that eviction with a saturating drop counter.

Event names and sources are measured with `std::string::size()` before they are
copied into retained storage, so the public 1,024-byte bound is a UTF-8 byte
bound. If either field exceeds that bound, the complete publication is dropped
without truncation. The attempt still consumes one valid sequence and adds one
drop. After sequence exhaustion, each publication attempt adds only the single
exhaustion drop and cannot wrap either the sequence or drop counter.

`Host::drain_compute_events(session, limit)` accepts limits from 1 through
1,024 and returns `ComputeEventBatch`: sequenced `events`, `next_sequence`,
`has_more`, and `dropped_count`. A successful call removes only the returned
oldest page and atomically resets the shared drop counter, including for an
empty page. Invalid limits return `GraphErrc::InvalidParameter` before any
removal or reset. The event service reserves result capacity before moving an
event, so `std::bad_alloc` cannot remove an event that the caller did not
receive. CLI consumers repeatedly request the maximum bounded page only while
`has_more` is true, but one polling pass has a fixed eight-page budget derived
as `ceil(8192 / 1024)`. This covers a complete retained production ring when no
producer races the pass, prevents a live producer from extending one pass
indefinitely, and leaves deferred or newly published events for a future poll.
No unbounded Host vector drain exists.

`TimingCollector` separately stores node timings and total elapsed compute time
when timing is enabled. Debug metadata in `NodeOutput` records worker id,
timestamp, execution time, device, and optional range checks.

## Error Handling

Compute failures throw `GraphError` with `GraphErrc` categories where possible.
Synchronous Kernel paths store a mutex-protected per-graph `LastError` for
best-effort observation. An asynchronous work item instead returns its own
`AsyncComputeResult` containing the exact failure code/message and only mirrors
that value into `LastError`; its Host future never reconstructs status from the
mutable mirror. The embedded adapter maps these values to public
`OperationStatus`, `Result<T>`, or `ps::Host::last_error()` values. Frontends
observe only the public Host surface and never inspect Kernel or
`InteractionService` directly.

Scheduler-admission failures occur at graph load or replacement rather than in
the ready-task loop. An invalid above-eight request or unknown type maps to
`InvalidParameter`; exhaustion of the fixed process worker ledger preserves
`GraphErrc::ComputeError` through embedded Host and IPC status boundaries.

## Boundaries and Rationale

- One request plan supplies both sequential and parallel execution semantics;
  the execution strategy changes mechanics, not topology or dirty meaning.
- One non-realtime HP request owns one `ComputeRun` from pre-planning
  descriptor capture through exact terminal publication. The Run owns full-plan
  temporary results or standalone dirty HP staging. Scheduler-backed full HP
  callbacks retain stable Run leases and matching composite task identity, but
  the Run does not own Graph state, workers, or the meaning of dependency
  transitions.
- `GraphStateExecutor` protects visible graph coherence, while
  `SchedulerTaskRuntime` receives only ready compute work. Graph-state commands
  therefore never become scheduler tasks.
- HP cache and RT proxy state use separate staged commit paths, so preview
  state cannot become authoritative HP output by implication.
- Scheduler epochs reject stale queued callbacks only and are not Run identity.
  Full HP task failures route through `(RunId, RunLocalTaskId)` under a stable
  lease. The current Run records QoS and a topology-only submission revision
  but has no authoritative graph revision, supersession, enforced deadline, or
  cooperative cancellation contract.

These separations keep planning, physical dispatch, and visible commit
independently testable. [ADR 0001](../adr/0001-graph-state-access-is-not-scheduler-dispatch.md)
governs the current graph-state/dispatch distinction. The accepted
[ADR 0007](../adr/0007-compute-runs-and-process-execution-have-separate-owners.md)
defines both the current bounded non-realtime HP Run/lease/completion-isolation
slice and the later independent paired HP/RT Runs, deterministic `RunGroup`
settlement, RT-first commit gate, admitted-Run registry, and broader lifecycle
ownership, while the
[process execution domain target](../roadmap/Kernel-Evolution.md#process-execution-domain)
describes the later revision and cancellation boundary without making it part
of the current flow.

## Implementation and Validation Entry Points

- `src/lib/runtime/kernel_compute.cpp`
- `src/lib/compute/compute_service.*`
- `src/lib/compute/compute_run.*`
- `src/lib/compute/compute_dispatch_plan_builder.*`
- `src/lib/compute/compute_task_dispatcher.*`
- `src/lib/compute/intent_update_coordinator.*`
- `src/lib/compute/dirty_update_executor.*`
- `src/lib/runtime/graph_event_service.*`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_scheduler.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/unit/test_compute_run.cpp`
- `tests/unit/test_event_stream_boundaries.cpp`
