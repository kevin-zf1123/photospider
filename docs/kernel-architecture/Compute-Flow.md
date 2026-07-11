# Kernel Compute Flow

This document describes the current compute paths and the target scheduler
direction.

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
instances.

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
`ComputeService::Request`, which carries only node target, cache, telemetry,
intent, and dirty ROI data. Parallel/runtime selection is carried separately as
`ComputeService::ExecutionStrategy`.

The CLI/REPL frontend is a permanent batch-oriented surface. It does not expose
RT intent commands, dirty ROI creation, or dirty source lifecycle commands such
as `compute rt`, `--dirty-roi`, `dirty begin`, `dirty update`, or `dirty end`.
`RealTimeUpdate` and dirty source lifecycle APIs remain kernel/test and future
GUI/WebUI-style frontend contracts, and the CLI must not be treated as the
production realtime control surface.

`GraphTraversalService` is topology-only. It provides traversal order and
explicit upstream/downstream topology queries from `GraphModel` adjacency.
Dirty-region demand and ROI projection use `RoiPropagationService`, while
formal propagation extents come from `GraphExtentResolver`.

Target compute planning flow after the `ComputeService` split separates
request-scoped static planning from per-update dirty work selection:

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
  -> TaskSubmissionPlan / ComputeTaskDispatcher
  -> task pools / scheduler / execution resources
```

`FullTaskGraphExpander` expands the raw graph into the full node/tile task graph
for one compute domain. It does not depend on the request target, cache state,
or dirty snapshot. `NodeCacheTaskGraphPruner` then prunes that graph to the
request target/dependency cone and records cache availability for selected
nodes. Dirty updates add a separate `DirtySnapshotTaskGraphPruner` pass that
annotates the selected graph with dirty metadata and produces the active
`DirtyUpdateWorkSet`.

Single-threaded and parallel execution should share the same pruned
`ComputePlan` or `ComputeTaskGraph`. Execution modes should differ in task
pools, scheduler policy, and execution resources, not in graph-level dirty
propagation, full task expansion, or task-graph pruning.

`ComputePlan` is a static analysis for the current compute request and domain.
It is derived while graph state is stable and remains the topology contract for
that request, whether the current commit policy writes directly to visible graph
state or a future commit policy stages buffers before commit. Dirty updates do
not rebuild topology semantics. They use the current `DirtyRegionSnapshot` and
dirty ROI to activate or clip a `DirtyUpdateWorkSet` from the plan for each HP
or RT update queue.

The request plan must enumerate the real compute tasks available to the
request, including tile tasks when the selected implementation is tiled. Dirty
state only prunes or activates tasks from that enumerated graph. It must not
expand new tile tasks during dirty clipping; this keeps full-frame tiled
parallelism and dirty ROI execution on the same task model.

The request's `ComputeTaskGraph` is immutable while scheduler tasks derived
from it may still run. Continuous RT dirty updates create new
`DirtyUpdateWorkSet` generations from the same plan and the latest dirty
snapshot, then submit those generations as priority-tagged task graph
submissions. They must not destruct and replace the task graph underneath an
active scheduler runtime.

Execution granularity is a separate layer. A graph can contain multiple nodes,
and each node/operator implementation can be monolithic or tiled. Tiled
execution can further use macro or micro task granularity. These choices are
node execution details and are independent from HP/RT intent semantics.

HP/RT compute domain and Micro/Macro granularity are orthogonal. The current
implementation defaults favor RT Micro_16 for interactive work and HP
Macro_256 for throughput work, but the model still has four distinct
domain/granularity cases:

| Case | Current tile size | Meaning |
| --- | --- | --- |
| `rt-micro` | 16x16 in RT proxy space | Low-latency RT proxy tile. |
| `rt-macro` | 64x64 in RT proxy space | Coarser RT proxy tile or aggregated RT work. |
| `hp-micro` | 64x64 in HP full-resolution space | Small HP tile. |
| `hp-macro` | 256x256 in HP full-resolution space | Throughput-oriented HP tile. |

`rt-macro` and `hp-micro` currently share the numeric size 64x64, but they are
not the same task type because they live in different coordinate spaces and
different task pools. A compute plan must not create dependencies from RT tasks
to HP tasks or from HP tasks to RT tasks. Realtime intent coordinates separate
HP and RT sibling work; scale conversion is used only to represent
corresponding ROIs, downsample state, or inspection data.

## Compute Intents

The kernel recognizes two formal compute intents:

| Intent | Meaning |
| --- | --- |
| `GlobalHighPrecision` | Full-quality HP compute. Owns high-precision output. Non-realtime compute enables only this HP path. |
| `RealTimeUpdate` | Interactive realtime update. Requires a dirty ROI and enables the HP/RT dual path. |

The intent model is formal. `ComputeService` remains the compute facade and
planning boundary, while parallel planned work is dispatched through the
configured `IScheduler` task runtime for each intent.

HP/RT dual path semantics belong to realtime intent, not to the parallel
execution mode. In realtime mode, HP computes the full-size authoritative node
work while RT computes the downscaled proxy, currently one quarter of width and
height, or one sixteenth of the pixel count. HP and RT work should be treated
as separate intent task pools. Scheduler/resource policy then decides how each
pool is executed: HP may use a single-thread scheduler, RT may use a GPU
scheduler, and realtime and non-realtime modes may use different scheduler
configuration.

The in-place `ComputeService` split is documented in
`Compute-Service-Split.md`. The current implementation keeps the internal facade
and routes internal work through compute-service collaborators.

## Sequential Compute

Sequential compute uses recursive dependency resolution:

1. Validate the target node.
2. Resolve traversal order and optionally clear caches.
3. For each dependency, compute upstream nodes recursively.
4. Build `runtime_parameters` from static parameters and parameter inputs.
5. Resolve an operation implementation for HP intent.
6. Execute monolithic or tiled operation.
7. Store output, emit events, update timing, and save disk cache when enabled.

Sequential compute is useful for simple execution and debugging. It now creates
the same internal full-expansion and node/cache-pruned task graph semantics as
the parallel path before executing the recursive path.

## Parallel Compute

Parallel compute derives a `ComputePlan` by expanding the full task graph and
then pruning it with `NodeCacheTaskGraphPruner` from `topo_postorder_from`.
`ComputeDispatchPlanBuilder` records that cache-pruned plan for inspection.
`TaskSubmissionPlan` materializes the plan's `ComputeTaskGraph` into scheduler
closures, dependency counters, ready handles, operation variants, and temporary
result slots, then submits ready node tasks through the configured scheduler's
`SchedulerTaskRuntime`. Tiled operations may spawn micro-tasks and increment
scheduler-owned completion counters.

`ComputeTaskDispatcher` keeps plan execution, dependency accounting, sparse
node-id mapping, temporary result storage, event logging, exception
propagation, and final target selection inside the compute-service boundary. It
dispatches already-planned work through scheduler task-runtime queues; it does
not make the scheduler own dirty propagation, compute-task derivation, or the
task graph itself. If the pruned planned dispatch is empty while the target has
no reusable HP output, the dispatcher reports a planning contract error instead
of falling back to recursive sequential compute.

For dirty execution, the dispatcher should materialize only the active
`DirtyUpdateWorkSet` selected from the request's `ComputeTaskGraph` by the
current dirty snapshot. Runtime dependency counters and ready-task queues are
execution artifacts; they are not stored in `DirtyRegionSnapshot` and are not
owned by the scheduler.

Dirty-region signals are node-originated state updates, not compute triggers. A
`DirtyRegionNode` should expose lifecycle state such as begin dirty-region
creation, update dirty region with the current ROI, and end dirty-region
creation. Frontend brush input may update a node, and a computed node may
discover new dirty state, but the dirty region is still emitted by a graph node.
A compute request may be created after the dirty region is closed, or an active
realtime request may coalesce updates while the dirty node is still changing.
The dispatcher's realtime cutoff must account for the dirty-node lifecycle and
the currently running work; an empty ready queue is not by itself proof that the
interaction has finished.

Dirty-node lifecycle updates enter a serialized `DirtyControlLane` that updates
dirty source state in the graph-scoped dirty snapshot, runs propagation to
refresh `actual_dirty_region`, and returns wakeup/cutoff decisions to internal
compute-service materialization. The snapshot stores `dirty_updating_count`;
`DirtyControlLaneResult` copies that count and uniquely owns the wakeup/cutoff
decisions. All remain internal evidence. The embedded Host adapter copies only
the public `DirtyRegionInspectionSnapshot` returned by lifecycle methods, not
those control fields. The scheduler receives only ready task callbacks with
epoch/generation metadata and optional scheduler-specific hints; it does not
receive task graphs, own the dirty control lane, or own compute-service dirty
queues.

## Graph-State Access and Commit Policy

Graph-state operations such as YAML loading, cache commands, inspection, and
ROI projection are operations on the visible `GraphModel`. They are not
compute-task dispatch and should not be routed through `SchedulerTaskRuntime`.

The current default is per-graph exclusive access through
`GraphStateExecutor`: graph-state operations and compute requests for the same
graph do not concurrently read or mutate the visible `GraphModel`. This includes
scheduler-backed parallel compute; the outer Kernel request enters
`GraphStateExecutor`, while ready node/tile callbacks are dispatched through
the scheduler runtime inside that boundary. This keeps graph topology, cache
fields, dirty snapshots, timing, and node runtime state coherent without
routing non-compute commands through scheduler queues.

Future work may add a `ComputeCommitPolicy` separate from `ComputeIntent`.
The current dirty update implementation already uses staged output commits for
HP/RT sibling safety: HP dirty workers write `HighPrecisionDirtyWriteBuffer`
and commit to the visible `GraphModel` only after the RT sibling has committed;
RT dirty workers write `RealtimeProxyWriteBuffer` and commit to the
runtime-owned `RealtimeProxyGraph`. A future `StagedInterruptibleCommit` policy
would extend this boundary with cancellation before commit and discard
uncommitted buffers on cancellation. This policy is intentionally not part of
`ComputeIntent`, because HP/RT intent semantics are independent from commit and
interruption behavior.

## GlobalHighPrecision

`GlobalHighPrecision` is the full-quality path. Without a dirty ROI it performs
normal full compute. With a dirty ROI it enters the HP dirty update path instead
of the former unconditional full-recompute fallback.

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

Dirty-region state planning now runs through the graph-scoped
`DirtyRegionPlanner`, and the resulting `DirtyRegionSnapshot` feeds dirty
work-set materialization and interaction-facing inspection summaries.

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
output commit as separate contracts so future task pools or modes can reuse the
same boundaries with their own domain.

The passed dirty ROI is converted into graph-scoped planner state for the
current request. Public `ps::Host` begin/update/end methods translate through the
embedded adapter to internal `Kernel` / `InteractionService` dirty-source
lifecycle methods, so frontend or node-facing code writes state through the same
graph-owned boundary. TODO: node-local dirty reports should become the origin
source for future frontend-driven dirty-region updates.

RT task graph expansion is domain-aware. When an operation has distinct HP and
RT metadata, the `RealTimeUpdate` plan uses RT metadata for tile size and
dependency ROI planning, while the HP sibling uses HP metadata. This keeps RT
Micro_16 planning independent from HP Macro_256 throughput defaults.

TODO: design the node-to-backend realtime dirty-report boundary. The design must
define how nodes emit realtime events, dirty regions, and update requests; which
layer owns dirty-region generation; how the internal `InteractionService` stays
separate from node and compute ownership; and how public Host subscribers let a
future GUI consume those events without turning the CLI into a realtime
interaction surface.

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
Internal `Kernel` catches these errors and stores a per-graph `LastError` for
the embedded Host adapter. The adapter maps that diagnostic to public
`OperationStatus`, `Result<T>`, or `ps::Host::last_error()` values; frontends
observe only the public Host status/error surface and never inspect Kernel or
`InteractionService` directly.
