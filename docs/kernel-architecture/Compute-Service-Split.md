# ComputeService Split Plan

This document records the in-place split of `ComputeService`. The first split
has landed behind the existing public facade. Items still marked TODO are
intentionally deferred to later scheduler, traversal, cache-migration, or
planner-plugin changes.

## Current Problem

`ComputeService` is the public compute entry point, but its implementation also
contains dependency resolution, cache policy, monolithic and tiled dispatch,
dirty-region state planning, compute task derivation, runtime queue
orchestration, timing, benchmark events, and output debug metadata.

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
  -> ParallelGraphExecutor
  -> ComputeMetricsRecorder
```

## Planned Boundaries

| Boundary | Responsibility | Status |
| --- | --- | --- |
| `ComputeService` facade | Preserve current public compute entry points and construct internal collaborators. | Implemented |
| `ComputeCachePolicy` | Centralize HP/RT/legacy cache selection. | Implemented in `src/kernel/services/compute-service/compute_cache_policy.*` |
| `NodeInputResolver` | Build runtime parameters and collect ready image inputs. | Implemented in `src/kernel/services/compute-service/node_input_resolver.*` |
| `NodeExecutor` | Execute monolithic and tiled operators consistently. | Implemented in `src/kernel/services/compute-service/node_executor.*` |
| `DirtyRegionPlanner` | Build graph-scoped dirty-region state from node-local dirty reports and operator propagation. | Implemented in `src/kernel/services/compute-service/dirty_region_planner.*` |
| `DirtyRegionSnapshot` | Enumerate dirty tiles, dirty monolithic nodes, per-node dirty ROIs, and per-edge ROI mappings using stable ids instead of raw pointers. | Implemented as an internal snapshot model |
| `ComputeTaskPlanner` | Convert compute requests and dirty snapshots into shared `ComputePlan` / `ComputeTaskGraph` semantics. | Implemented as an internal planning boundary; plugin ABI remains TODO |
| `IntentUpdateCoordinator` | Coordinate `GlobalHighPrecision` and `RealTimeUpdate` intent semantics, including realtime HP/RT dual path behavior independent from execution mode. | Implemented in `src/kernel/services/compute-service/intent_update_coordinator.*` |
| `ParallelGraphExecutor` | Encapsulate the current legacy `GraphRuntime` queue DAG path. | Implemented in `src/kernel/services/compute-service/parallel_graph_executor.*` |
| `ComputeMetricsRecorder` | Centralize events, timings, benchmark events, and debug metadata. | Implemented in `src/kernel/services/compute-service/compute_metrics_recorder.*` |

## Cache Rules

The split must preserve the existing cache contract:

- `cached_output_high_precision` is the only formal reusable cache.
- `cached_output_real_time` is transient interactive state.
- `cached_output` is legacy migration residue and should not receive new
  writes.

TODO: remove or hide legacy `cached_output` fallback in a later change after
all HP call sites are verified.

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

Realtime HP/RT dual path selection is not an execution mode. Non-realtime
requests enable the HP path only. `RealTimeUpdate` requests enable both HP and
RT work for the dirty ROI regardless of whether the caller selected
single-threaded, parallel, GPU, or another scheduler/resource policy. The
current implementation coordinates this through `IntentUpdateCoordinator`; the
legacy runtime queue can submit HP and RT work concurrently, while
single-threaded execution runs the same intent work inline.

TODO: planner plugin ABI remains explicitly deferred to a later change.

## Scheduler Boundary

The current parallel compute path still uses legacy `GraphRuntime` queues and
completion counters. That behavior is now isolated behind
`ParallelGraphExecutor`.

Schedulers should pull planned or annotated tasks from intent-aware task pools
and schedule compute resources. They should not own graph-level dirty
propagation or compute-task derivation. Existing `IScheduler::schedule_node`,
scheduler-local tile splitting, and task-group aggregation are migration
interfaces.

The target task-pool model has separate HP and RT pools with independently
selectable scheduler configuration. For example, HP can use a single-thread
scheduler while RT uses a GPU scheduler. Realtime and non-realtime modes can
also use different scheduler configuration. This split does not implement full
scheduler-owned HP/RT task-pool routing; it only keeps the intent boundary from
being coupled to the legacy parallel executor.

TODO: route planned tasks through scheduler-owned task pools in a later
scheduler-focused change.

## Traversal Boundary

Dirty-region planning should continue to call current
`GraphTraversalService::compute_upstream_roi` and related traversal APIs.

TODO: split `GraphTraversalService` topology traversal from ROI/spatial
propagation in a separate change.

## Interaction Boundary

`InteractionService` is the frontend-facing facade between CLI/TUI/frontends and
the kernel. In the dirty-region context, it should expose graph-scoped dirty
snapshot inspection and visualization APIs. It is not the authoritative source
of dirty-region generation or propagation.

`InteractionService` now exposes a dirty snapshot debug summary for inspection.

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
