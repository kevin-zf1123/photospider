# ComputeService Split Plan

This document records the in-place split of `ComputeService`. The first split
has landed inside the backend, behind the installable public `ps::Host` facade.
Items still marked TODO are intentionally deferred to later scheduler,
traversal, cache-migration, or planner-plugin changes.

## Current Problem

`ComputeService` is the internal compute facade reached after the public Host
boundary, but its implementation also contains dependency resolution, cache
policy, monolithic and tiled dispatch, dirty-region state planning, compute
task derivation, scheduler task-runtime dispatch coordination, timing,
benchmark events, and output debug metadata.

The split should preserve behavior first. It is not a rewrite of the graph
engine and it is not a plugin ABI change.

The compute path now uses structured request objects at each boundary.
Frontends submit a public `ps::HostComputeRequest` to `ps::Host`; the embedded
Host adapter translates that value into an internal `Kernel::ComputeRequest`.
`ComputeService::Request` then receives only target node, cache, telemetry,
intent, and dirty ROI data, while `ComputeService::ExecutionStrategy` carries
the runtime and scheduler-backed execution policy. This keeps CLI behavior
stable without extending long positional boolean parameter lists.

## Target Shape

```text
ComputeService facade
  -> ComputeCachePolicy
  -> NodeInputResolver
  -> FullTaskGraphExpander
  -> FullTaskGraph
  -> NodeCacheTaskGraphPruner
  -> ComputePlan / pruned ComputeTaskGraph
  -> ComputeDispatchPlanBuilder
  -> TaskPopulationStrategy / task dependency population helpers
  -> DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> DirtySnapshotTaskGraphPruner
  -> IntentUpdateCoordinator
  -> ComputeTaskDispatcher
  -> TaskSubmissionPlan / dispatch_planned_tasks
  -> DirtyUpdateWorkSet
  -> NodeExecutor
  -> ComputeMetricsRecorder
```

## Planned Boundaries

| Boundary | Responsibility | Status |
| --- | --- | --- |
| `ComputeService` facade | Accept structured compute requests, preserve existing behavior, and construct internal collaborators. | Implemented |
| `ComputeCachePolicy` | Centralize HP/RT cache selection. | Implemented in `src/lib/compute/compute_cache_policy.*` |
| `NodeInputResolver` | Build runtime parameters and collect ready image inputs. | Implemented in `src/lib/compute/node_input_resolver.*` |
| `NodeExecutor` | Execute monolithic and tiled operators consistently. | Implemented in `src/lib/compute/node_executor.*` |
| `DirtyRegionPlanner` | Build graph-scoped dirty-region state from node-local dirty reports and operator propagation. | Implemented in `src/lib/compute/dirty_region_planner.*` |
| `DirtyRegionSnapshot` | Enumerate dirty tiles, dirty monolithic nodes, per-node dirty ROIs, and per-edge ROI mappings using stable ids instead of raw pointers. | Implemented as an internal snapshot model |
| `FullTaskGraphExpander` | Expand the raw graph into a full node/tile `FullTaskGraph` for one compute domain without using request target, cache state, or dirty snapshot. | Implemented in `src/lib/compute/task_graph_planning.*` |
| `NodeCacheTaskGraphPruner` | Prune a `FullTaskGraph` to the request target/dependency cone and record selected node cache availability. | Implemented in `src/lib/compute/task_graph_planning.*` |
| `ComputeDispatchPlanBuilder` | Build and record the cache-pruned high-precision plan used by scheduler-backed dispatcher execution. | Implemented in `src/lib/compute/compute_dispatch_plan_builder.*` |
| `TaskPopulationStrategy` and task population helpers | Populate graph-backed or graphless planned task records and task dependencies without using dirty snapshots to create new task shapes. | Implemented in `src/lib/compute/task_population_strategy.*` and `task_graph_planning.*` |
| `DirtySnapshotTaskGraphPruner` | Apply a `DirtyRegionSnapshot` to a node/cache-pruned `ComputeTaskGraph` and materialize the active `DirtyUpdateWorkSet`. | Implemented in `src/lib/compute/task_graph_planning.*`; plugin ABI remains TODO |
| `IntentUpdateCoordinator` | Coordinate `GlobalHighPrecision` and `RealTimeUpdate` intent semantics, including realtime HP/RT dual path behavior independent from execution mode. | Implemented in `src/lib/compute/intent_update_coordinator.*` |
| Connected-parameter preflight | Stabilize one request-local HP producer closure before dependent parameter, extent, ROI, and task planning; use scheduler initial-handle batches when parallel. | Implemented in `src/lib/compute/dirty_update_executor.*` and coordinated by `ComputeService` |
| `ComputeTaskDispatcher` | Execute node/cache-pruned task graph semantics by collecting source tasks, checking task-graph readiness, dispatching ready tasks through `SchedulerTaskRuntime`, and committing results. | Implemented in `src/lib/compute/compute_task_dispatcher.*` |
| `TaskSubmissionPlan` and `dispatch_planned_tasks` | Convert a cache-pruned plan into scheduler closures, dependency counters, ready handles, and empty-plan validation for one dispatcher call. | Implemented in `src/lib/compute/compute_task_submission.*` |
| `ComputeMetricsRecorder` | Centralize events, timings, benchmark events, and debug metadata. | Implemented in `src/lib/compute/compute_metrics_recorder.*` |

`NodeExecutor` keeps tiled input preparation outside the per-tile loop. The
`TiledInputNormalizer` helper materializes image_mixing secondary resize/crop
and channel conversions once per node invocation, then `NodeExecutor` reuses the
normalized context while building read-only `InputTile` views and writable
`OutputTile` views for each tile task. This boundary avoids casting upstream
`NodeOutput` buffers to mutable tile inputs and prevents whole-input
normalization from repeating per tile.

## Cache Rules

The split must preserve the existing cache contract:

- `cached_output_high_precision` is the only formal reusable cache.
- `RealtimeProxyGraph` owns transient RT interactive state outside `GraphModel`.

Formal HP writes and disk cache authority are handled through
`cached_output_high_precision`; RT output is never promoted to reusable cache
authority.

## Dirty-Region Boundary

Dirty regions originate from node-local changes, but propagation semantics are
an operator contract. Operators should explicitly define dirty and forward
propagation behavior, possibly using node parameters, spatial metadata, cached
dependency information, or data-dependent LUTs. Current identity propagation
fallback is migration support and should not be treated as sufficient for new
operators.

`DirtyRegionPlanner` owns graph-scoped dirty-region state for the current HP and
RT dirty update paths. It exposes a `DirtyRegionSnapshot` using stable node ids,
tile coordinates, pixel ROIs, graph generation metadata, and edge mappings.
`ComputeService` stores an inspection summary on the graph. The internal
`InteractionService` returns it to the embedded Host adapter, which copies it
into public Host values for frontend/debug queries.

`DirtyRegionSnapshot` is graph-scoped state alongside graph topology, not a
scheduler graph. It records the current dirty facts used to activate or clip
work from a request-scoped `ComputeTaskGraph`.

Dirty-region lifecycle is separate from compute triggering. Dirty signals are
node-originated: frontend interaction may feed a node, and compute may cause a
node to discover dirty state, but the graph-scoped snapshot is updated through
that node's begin/update/end dirty-region lifecycle. Dirty signals should not
automatically enqueue a compute request. This allows an interaction such as a
brush stroke to accumulate or stream ROI updates through its node without
rebuilding a full compute plan for every stamp event.

Dirty lifecycle updates should pass through a serialized `DirtyControlLane`
owned by the dirty-state/executor boundary. The control lane updates
`dirty_source_nodes` and source lifecycle state. `dirty_updating_count` is
derived from that lifecycle state, and the propagator derives the propagated
`actual_dirty_region` snapshot from the source set, then wakes materialization.
It should not be modeled as a scheduler-owned compute task queue or as
node-local compute ownership.

Current production-facing lifecycle writes enter through public `ps::Host`
begin/update/end dirty-source methods. The embedded adapter translates them to
internal `Kernel` / `InteractionService` methods, which serialize graph-state
mutation through `GraphStateExecutor` and route node/frontend lifecycle events
through `DirtyControlLane`. The lane reuses `DirtyRegionPlanner` for membership,
lifecycle, and actual dirty ROI refresh. `dirty_updating_count` is stored in the
internal `DirtyRegionSnapshot` and copied into `DirtyControlLaneResult`; the
wakeup/cutoff decisions exist only in `DirtyControlLaneResult`. All three remain
internal evidence consumed by compute-service work-set materialization. The
embedded adapter calls the
snapshot-returning internal lifecycle methods and copies only the public dirty
snapshot fields. Frontend event subscription and reusable request-plan
coalescing remain follow-up work.

Current constraint: `DirtyControlLane` does not own or cache a
`ComputeTaskGraph`. It records graph-scoped dirty lifecycle state and wakeup
intent; each compute request still materializes work from the current snapshot
inside the compute-service planning boundary. Reusing one immutable request
plan across multiple dirty generations remains a later optimization, not a
scheduler responsibility.

TODO: add richer dirty snapshot visualization after the frontend has a concrete
mask/tile rendering contract.

## Compute Task Planning Boundary

Task graph planning is split into explicit expansion and pruning boundaries.
`FullTaskGraphExpander` expands the raw graph into a full node/tile task graph
for one compute domain. This full expansion does not depend on request target,
node cache state, or dirty snapshot. It answers only "what executable node/tile
tasks exist for this graph and domain?"

There is no current single planner class that owns all plan creation. The
current implementation is a module chain: `ComputeDispatchPlanBuilder` derives
the request traversal and records the high-precision plan,
`FullTaskGraphExpander` enumerates full-domain task shapes,
`NodeCacheTaskGraphPruner` narrows them to the request/cache cone,
`TaskPopulationStrategy` and task dependency helpers populate executable task
records, and `DirtySnapshotTaskGraphPruner` later activates dirty work from the
already-pruned graph.

`GraphModel` caches immutable `FullTaskGraph` expansions by topology
generation, compute intent, and task-shape configuration. A `force_recache`
request clears that cache before planning because input data or source
parameters may change output extents without changing graph topology; tiled
task ROIs must therefore be rebuilt from current extents instead of a previous
expansion.

`NodeCacheTaskGraphPruner` consumes that `FullTaskGraph`, the requested target
node/dependency cone, and current node/cache state, then emits the pruned
`ComputePlan` / `ComputeTaskGraph` used by sequential and parallel execution.
It records cache availability for selected nodes while preserving the existing
execution contract that cache hits are resolved during task execution.

`DirtySnapshotTaskGraphPruner` is a separate dirty-source pruner. It consumes a
node/cache-pruned `ComputeTaskGraph` plus a `DirtyRegionSnapshot`, annotates the
selected graph with dirty metadata, and materializes the active
`DirtyUpdateWorkSet`. It may clip or activate already-expanded tasks, but it
must not create new tile or node task shapes. This preserves full-frame tiled
compute as the same task model used by HP and RT dirty updates.

Realtime HP/RT dual path selection is not an execution mode. Non-realtime
requests enable the HP path only. `RealTimeUpdate` requests enable both HP and
RT work for the dirty ROI regardless of whether the caller selected
single-threaded, parallel, GPU, or another scheduler/resource policy. The
current implementation coordinates this through `IntentUpdateCoordinator`.
When both scheduler task runtimes are available, the coordinator starts the RT
dirty sibling first, starts the HP sibling second, waits for RT first, and lets
the sibling commit gate keep HP `GraphModel` mutation behind RT proxy commit.
Without scheduler runtimes, the same work runs inline in RT-then-HP order.

The HP and RT paths keep separate single-domain plans and dirty snapshots. The
HP path uses a `GlobalHighPrecision` plan and normally clips HP work from an HP
dirty snapshot. When HP dirty execution is forced, it expands the HP planning
ROI to the target node's full current HP extent because the HP staging buffer is
not seeded from old pixels. The RT path uses a `RealTimeUpdate` plan and clips
RT work from an RT dirty snapshot. RT dirty node execution stages proxy output
through `RealtimeProxyWriteBuffer` and commits it to `RealtimeProxyGraph` only
after the RT work set drains. HP dirty node execution stages HP output through
`HighPrecisionDirtyWriteBuffer` and commits it to `GraphModel` after the RT gate
opens. A single full graph expansion or pruner pass must not emit both HP and RT
task pools. Future task-pool extensions should keep this per-domain expansion,
pruning, and commit pattern.

TODO: planner plugin ABI remains explicitly deferred to a later change.

## Scheduler Boundary

The target parallel compute path dispatches already-planned graph work through
the scheduler selected for the request's `ComputeIntent`.
`ComputeTaskDispatcher` owns compute-plan execution, internal DAG counters,
temporary result storage, tile micro-task accounting, exception propagation,
and final output selection, but hands concrete ready task callbacks to
`SchedulerTaskRuntime`.

Before HP or RT dirty planning, a target cone with connected parameter inputs
is stabilized by the compute-service boundary. The host executes each required
parameter producer and its upstream closure exactly once into one immutable,
request-local HP snapshot. Effective-parameter conversion, allocation,
staged-source lookup, and immutable operation input-view construction complete
before each callback is entered. The same snapshot then drives dependent
parameter values, current output extents, dirty/forward ROI propagation, and
node/tile task shapes for the request's HP and RT siblings.

On a scheduler-backed request, each topologically ready preflight node is one
valid `TaskHandle` in a non-empty initial batch. Its completion wait settles
before the next node or phase-two dispatch. A failure publishes no staged HP
cache/version/ROI, RT proxy result, or downstream work; an admitted batch is
settled before the exception returns. Retry may reuse the same runtime
scheduler object, but constructs a fresh request snapshot, a fresh
request-local executor, a fresh initial batch/epoch, and fresh exception,
completion, and staged-output state. This host-publication guarantee does not
roll back external side effects from an operation callback that was already
entered. These request-planning rules are owned here; Plugin ABI documentation
owns only the per-callback public-value conversion and exception fence.

For full high-precision parallel dispatch, `TaskSubmissionPlan` converts the
cache-pruned plan into dense node indexes, dependency counters, scheduler task
handles, operation variants, and temporary result slots. `dispatch_planned_tasks`
then submits initial ready handles and validates empty plans. An empty plan is
legal only when the target already has reusable high-precision output; an
uncached empty target is a planning contract error rather than a recursive
sequential fallback.

For dirty updates, production HP and RT executors build request-local dirty
`TaskExecutor` handles from the request plan and dirty snapshot, then call the
internal class's public C++ static
`ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first` helper as the
source-first submission boundary. Public C++ access here only permits calls
across private backend translation units; it is not product public API. Runtime
dependency counters, task reference counts, and ready queues are owned inside
compute-service dispatcher state during that materialization. The scheduler
receives concrete ready task handles with scheduler priority and completion
accounting; it does not receive task graphs, own dirty-state lookup, or derive
task graphs.

Realtime materialization must consider both current running work and the dirty
node lifecycle. If a node is still creating dirty regions, an empty ready queue
should not force the realtime compute request to terminate and rebuild a new
full plan for the next ROI update. Source-node tasks for a dirty generation
are submitted source-first by the dispatcher helper before dependent downstream
dirty work is released, not as a separate dirty source queue and not as a
scheduler-wide priority contract. Later work may extract the work-set selection
logic behind a task-pruner plugin interface.

The dispatcher must treat `ComputeTaskGraph` as immutable once scheduler-visible
tasks have been derived from it. New dirty updates produce new
`DirtyUpdateWorkSet` generations from the same plan and latest snapshot, then
submit concrete ready task callbacks as work becomes ready. The dispatcher may
attach generation and epoch metadata, plus optional scheduler-specific hints,
but it should not destruct and replace a task graph concurrently with a running
scheduler runtime.

Schedulers should receive ready task callbacks from intent-aware dispatcher
paths, then schedule compute resources. They should not own task graphs,
graph-level dirty propagation, dependency counters, dirty work pruning, or
compute-task derivation. Compute-planning helpers have been removed from the
formal scheduler surface.

Scheduler behavior for newly submitted ready tasks is policy-specific: a
scheduler may drop stale queued work by epoch, route tasks through FIFO/LIFO or
work-stealing queues, prefer CPU/GPU resources, or let previous work finish if
that matches its latency/throughput policy. The scheduler's input should still
be concrete ready task callbacks with generic metadata such as epoch and dirty
generation. It should not need a
dirty-feature-specific queue.

The target task-pool model has separate HP and RT pools with independently
selectable scheduler configuration. For example, HP can use a single-thread
scheduler while RT uses a GPU scheduler. Realtime and non-realtime modes can
also use different scheduler configuration. Planned parallel work now reaches
scheduler-owned task runtimes after compute-service planning. Later
scheduler-focused work can add richer annotated task pools, planner plugin ABI,
and more detailed scheduler policy metadata.

## Traversal Boundary

Dirty-region planning now consumes the narrowed topology and propagation
boundaries directly. `GraphTraversalService` is topology-only and provides
traversal order plus explicit upstream/downstream topology queries backed by
`GraphModel` adjacency. `DirtyRegionPlanner` uses `RoiPropagationService` for
upstream ROI demand and `GraphExtentResolver` for HP-authoritative extent
resolution. Traversal no longer exposes ROI projection, upstream ROI
computation, dependency-tree formatting, or compatibility wrappers for removed
APIs.

Dependency-tree inspection is structured: `GraphInspectService` builds
dependency-tree snapshots from topology adjacency, the internal
`Kernel`/`InteractionService` returns them to the embedded Host adapter, and the
adapter copies public Host snapshots that CLI/TUI/frontend code renders.

## Host and Internal Interaction Boundary

`ps::Host` is the frontend-facing interface used by CLI/TUI and embedded
frontends. `InteractionService` is an internal wrapper between the embedded Host
adapter and `Kernel`; frontend code neither includes nor calls it directly. The
dirty-region boundary exposes copied graph-scoped inspection and lifecycle
values through Host, while neither Host nor `InteractionService` is the
authoritative source of dirty-region generation or propagation.

Internally, `DirtyRegionSnapshot` retains `dirty_updating_count`, and
`DirtyControlLaneResult` copies that count while adding dispatcher wakeup intent
and downstream-completion cutoff decisions. Only those two decisions are
specific to the control result. None of these internal fields crosses the
public boundary. `InteractionService` returns the current dirty snapshot to the
embedded adapter for public lifecycle methods; Host copies only fields present
in `DirtyRegionInspectionSnapshot`: generation, source lifecycle/ROIs, dirty
tiles, monolithic regions, propagated ROIs, and edge mappings. Host also returns
structured dependency-tree and graph-inspection snapshots so frontends can
parse graph structure before choosing a presentation format.

The public Host exception boundary converts recoverable graph, parser,
filesystem, standard, and unknown backend failures into `OperationStatus` or
`Result<T>` failures. Resource exhaustion is deliberately different:
`std::bad_alloc` may propagate from every non-destructor Host method, and an
async compute future may rethrow it when consumed. The installable Doxygen
contract records this behavior method-by-method; callers must not treat Host as
unconditionally non-throwing.

The CLI/REPL frontend is a permanent batch-oriented surface. It does not expose
RT intent commands, dirty ROI creation, or dirty source lifecycle commands such
as `compute rt`, `--dirty-roi`, `dirty begin`, `dirty update`, or `dirty end`.
`RealTimeUpdate` and dirty source lifecycle APIs remain kernel/test and future
GUI/WebUI-style frontend contracts.

TODO: design the missing node-to-backend interface for realtime dirty updates.
The interface must let nodes provide dirty-region lifecycle events, realtime
update events, and update requests while keeping the internal
`InteractionService` separate from dirty-region generation and compute
scheduling ownership; frontend delivery remains a public Host responsibility.

TODO: add richer visualization APIs after the graph-scoped dirty state has a
frontend display contract.

## Global HP Dirty ROI

Global HP compute with a dirty ROI routes through HP dirty planning. The HP
dirty update accepts dirty ROI and normally clips HP work from the
high-precision task graph so large graphs and large images do not pay for
unaffected work. If `force_recache=true`, the executor instead plans the full
target HP frame before commit because staging starts empty and cannot preserve
ROI-external pixels from the previous HP cache. The coordinator records
`intent_coordinator_global_dirty_update` for this path.

## Validation Expectations

Each extraction step should have focused validation before duplicate logic is
removed from `compute_service.cpp`.

Required regression areas:

- cache semantics
- propagation contracts
- dirty-region tiled computation
- scheduler behavior
- kernel contracts

The OpenSpec change `split-compute-service` owns the detailed task list for the
implementation.
