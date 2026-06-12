# ComputeService Split Plan

This document records the in-place split of `ComputeService`. The first split
has landed behind the existing public facade. Items still marked TODO are
intentionally deferred to later scheduler, traversal, cache-migration, or
planner-plugin changes.

## Current Problem

`ComputeService` is the public compute entry point, but its implementation also
contains dependency resolution, cache policy, monolithic and tiled dispatch,
dirty-region state planning, compute task derivation, scheduler task-runtime
dispatch coordination, timing, benchmark events, and output debug metadata.

The split should preserve behavior first. It is not a rewrite of the graph
engine and it is not a plugin ABI change.

## Target Shape

```text
ComputeService facade
  -> ComputeCachePolicy
  -> NodeInputResolver
  -> NodeExecutor
  -> DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> ComputeTaskPlanner
  -> IntentUpdateCoordinator
  -> ComputePlanExecutor
  -> ComputeMetricsRecorder
```

## Planned Boundaries

| Boundary | Responsibility | Status |
| --- | --- | --- |
| `ComputeService` facade | Preserve current public compute entry points and construct internal collaborators. | Implemented |
| `ComputeCachePolicy` | Centralize HP/RT cache selection. | Implemented in `src/kernel/services/compute-service/compute_cache_policy.*` |
| `NodeInputResolver` | Build runtime parameters and collect ready image inputs. | Implemented in `src/kernel/services/compute-service/node_input_resolver.*` |
| `NodeExecutor` | Execute monolithic and tiled operators consistently. | Implemented in `src/kernel/services/compute-service/node_executor.*` |
| `DirtyRegionPlanner` | Build graph-scoped dirty-region state from node-local dirty reports and operator propagation. | Implemented in `src/kernel/services/compute-service/dirty_region_planner.*` |
| `DirtyRegionSnapshot` | Enumerate dirty tiles, dirty monolithic nodes, per-node dirty ROIs, and per-edge ROI mappings using stable ids instead of raw pointers. | Implemented as an internal snapshot model |
| `ComputeTaskPlanner` | Convert compute requests and dirty snapshots into shared `ComputePlan` / `ComputeTaskGraph` semantics. | Implemented as an internal planning boundary; plugin ABI remains TODO |
| `IntentUpdateCoordinator` | Coordinate `GlobalHighPrecision` and `RealTimeUpdate` intent semantics, including realtime HP/RT dual path behavior independent from execution mode. | Implemented in `src/kernel/services/compute-service/intent_update_coordinator.*` |
| `ComputePlanExecutor` | Execute `ComputeTaskPlanner` plan semantics by materializing task-graph work, dispatching ready tasks through `SchedulerTaskRuntime`, and committing results. | Implemented in `src/kernel/services/compute-service/compute_plan_executor.*` |
| `ComputeMetricsRecorder` | Centralize events, timings, benchmark events, and debug metadata. | Implemented in `src/kernel/services/compute-service/compute_metrics_recorder.*` |

## Cache Rules

The split must preserve the existing cache contract:

- `cached_output_high_precision` is the only formal reusable cache.
- `cached_output_real_time` is transient interactive state.

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
`ComputeService` stores an inspection summary on the graph, and
`InteractionService` exposes that summary for frontend/debug queries.

TODO: add richer dirty snapshot visualization after the frontend has a concrete
mask/tile rendering contract.

## Compute Task Planning Boundary

Single-threaded and parallel compute should share one logical `ComputePlan` or
`ComputeTaskGraph`. `ComputeTaskPlanner` consumes compute requests and dirty
snapshots, then produces internal `ComputePlan` semantics used by sequential,
parallel, HP, and RT paths before execution-specific dispatch. Execution modes
should differ only in task pools, scheduler policy, and resource selection.
No separate task-graph composer is introduced while this planner boundary stays
cohesive.

Realtime HP/RT dual path selection is not an execution mode. Non-realtime
requests enable the HP path only. `RealTimeUpdate` requests enable both HP and
RT work for the dirty ROI regardless of whether the caller selected
single-threaded, parallel, GPU, or another scheduler/resource policy. The
current implementation coordinates this through `IntentUpdateCoordinator`;
parallel execution submits HP and RT sibling work to their intent-specific
scheduler task runtimes, while single-threaded execution runs the same intent
work inline.

The HP and RT paths call the shared `ComputeTaskPlanner` separately. The HP
path creates a `GlobalHighPrecision` plan from its HP dirty snapshot, and the RT
path creates a `RealTimeUpdate` plan from its RT dirty snapshot. A single
`ComputeTaskPlanner` invocation must not emit both HP and RT task pools. Future
task-pool extensions should keep this per-domain planner invocation pattern.

TODO: planner plugin ABI remains explicitly deferred to a later change.

## Scheduler Boundary

The current parallel compute path dispatches already-planned graph work through
the scheduler selected for the request's `ComputeIntent`.
`ComputePlanExecutor` owns compute-plan execution, internal DAG counters,
temporary result storage, tile micro-task accounting, exception propagation,
and final output selection, but hands concrete task execution to
`SchedulerTaskRuntime`.

Schedulers should pull planned or annotated tasks from intent-aware task pools
or receive planned work through `SchedulerTaskRuntime`, then schedule compute
resources. They should not own graph-level dirty propagation or compute-task
derivation. Compute-planning helpers have been removed from the formal
scheduler surface.

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
dependency-tree snapshots from topology adjacency, `Kernel`/`InteractionService`
return those snapshots, and CLI/TUI/frontend code renders human-readable text.

## Interaction Boundary

`InteractionService` is the frontend-facing facade between CLI/TUI/frontends and
the kernel. In the dirty-region context, it should expose graph-scoped dirty
snapshot inspection and visualization APIs. It is not the authoritative source
of dirty-region generation or propagation.

`InteractionService` now exposes a dirty snapshot debug summary for inspection.
It also exposes structured dependency-tree and graph-inspection snapshots so
frontends can parse graph structure before choosing a presentation format.

TODO: add richer visualization APIs after the graph-scoped dirty state has a
frontend display contract.

## Global HP Dirty ROI

Current global HP compute with a dirty ROI may still trigger full recompute in
some entry paths. This split should document that behavior and avoid changing
it accidentally.

TODO: decide in a later change whether global HP dirty ROI should use optimized
partial HP update planning.

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
