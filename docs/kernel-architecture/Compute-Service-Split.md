# ComputeService Split Plan

This document records the planned in-place split of `ComputeService`. It is a
maintained architecture document for the active branch, but the implementation
is not complete yet. Items marked TODO are intentionally deferred or still
pending implementation.

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
| `ComputeService` facade | Preserve current public compute entry points. | TODO |
| `ComputeCachePolicy` | Centralize HP/RT/legacy cache selection. | TODO |
| `NodeInputResolver` | Build runtime parameters and collect ready image inputs. | TODO |
| `NodeExecutor` | Execute monolithic and tiled operators consistently. | TODO |
| `DirtyRegionPlanner` | Build graph-scoped dirty-region state from node-local dirty reports and operator propagation. | TODO |
| `DirtyRegionSnapshot` | Enumerate dirty tiles, dirty monolithic nodes, per-node dirty ROIs, and per-edge ROI mappings using stable ids instead of raw pointers. | TODO |
| `ComputeTaskPlanner` | Convert compute requests and dirty snapshots into shared `ComputePlan` / `ComputeTaskGraph` semantics. | TODO |
| `IntentUpdateCoordinator` | Coordinate `GlobalHighPrecision` and `RealTimeUpdate` branching. | TODO |
| `ParallelGraphExecutor` | Encapsulate the current legacy `GraphRuntime` queue DAG path. | TODO |
| `ComputeMetricsRecorder` | Centralize events, timings, benchmark events, and debug metadata. | TODO |

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

`DirtyRegionPlanner` should own graph-scoped dirty-region state. The state
should be exposed as a `DirtyRegionSnapshot` that uses stable node ids, tile
coordinates, pixel ROIs, graph generation metadata, and edge mappings. The
snapshot is the shared source for visualization and compute task planning.

TODO: introduce the graph-scoped dirty snapshot and operator propagation
validation; current code still passes dirty ROI through compute paths and
projection helpers directly.

## Compute Task Planning Boundary

Single-threaded and parallel compute should share one logical `ComputePlan` or
`ComputeTaskGraph`. `ComputeTaskPlanner` should consume compute requests and
dirty snapshots, then produce planned work. Execution modes should differ only
in task pools, scheduler policy, and resource selection.

TODO: define the internal compute task planning representation and keep planner
plugin ABI explicitly deferred to a later change.

## Scheduler Boundary

The current parallel compute path still uses legacy `GraphRuntime` queues and
completion counters in some paths. The first split should isolate that behavior
behind `ParallelGraphExecutor`.

Schedulers should pull planned or annotated tasks from intent-aware task pools
and schedule compute resources. They should not own graph-level dirty
propagation or compute-task derivation. Existing `IScheduler::schedule_node`,
scheduler-local tile splitting, and task-group aggregation are migration
interfaces.

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

TODO: add dirty snapshot query/visualization APIs after the graph-scoped dirty
state exists.

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
