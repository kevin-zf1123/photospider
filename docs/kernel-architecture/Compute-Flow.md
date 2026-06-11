# Kernel Compute Flow

This document describes the current compute paths and the target scheduler
direction.

## Entry Points

Typical frontend flow:

```text
CLI / TUI
  -> InteractionService
  -> Kernel
  -> GraphRuntime
  -> ComputeService
  -> OpRegistry / GraphCacheService / GraphTraversalService
```

`Kernel` owns the multi-graph API. `GraphRuntime` owns one graph model, event
service, worker state, platform context, and scheduler instances.

`InteractionService` is the frontend-facing facade for kernel interaction. Its
overall role is to decouple CLI/TUI/frontend commands from kernel internals. In
the dirty-region context, it should expose graph-scoped dirty snapshot queries
and visualization hooks; it is not the authoritative source of dirty-region
generation or propagation.

Target compute planning flow after the `ComputeService` split:

```text
ComputeService facade
  -> DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> ComputeTaskPlanner
  -> ComputePlan / ComputeTaskGraph
  -> task pools / scheduler / execution resources
```

Single-threaded and parallel execution should share the same logical
`ComputePlan` or `ComputeTaskGraph`. Execution modes should differ in task
pools, scheduler policy, and execution resources, not in graph-level dirty
propagation or compute-task derivation.

## Compute Intents

The kernel recognizes two formal compute intents:

| Intent | Meaning |
| --- | --- |
| `GlobalHighPrecision` | Full-quality HP compute. Owns high-precision output. |
| `RealTimeUpdate` | Interactive update. Requires a dirty ROI. |

The intent model is formal. The current implementation still has several paths
that bypass `IScheduler` and call `ComputeService` directly.

The in-place `ComputeService` split is documented in
`Compute-Service-Split.md`. The current implementation keeps the public facade
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
the same internal `ComputeTaskPlanner` plan semantics as the parallel path
before executing the recursive path.

## Parallel Compute

Parallel compute builds a DAG from `topo_postorder_from`, tracks dependency
counters, and submits ready node tasks to `GraphRuntime` worker queues. Tiled
operations may spawn micro-tasks and increment runtime completion counters.

This path is current behavior, but it is also part of the scheduler migration
surface. The formal long-term target is to route compute through `IScheduler`
instances after dirty-region state and compute-task planning have produced
planned work.

The legacy `GraphRuntime` queue path is isolated behind
`ParallelGraphExecutor`. TODO: full planned-task scheduler routing remains a
later scheduler-focused change.

## GlobalHighPrecision

`GlobalHighPrecision` is the full-quality path. Without a dirty ROI it performs
normal full compute. With current code, a dirty ROI on global compute may still
trigger full recompute in some entry paths.

HP dirty-region update computes a backward ROI plan, aligns dirty regions to HP
tile boundaries, updates affected HP tiles, records HP ROI/version metadata, and
can schedule downsample work to refresh RT transient state.

Dirty-region state planning now runs through the graph-scoped
`DirtyRegionPlanner`, and the resulting `DirtyRegionSnapshot` feeds compute
task planning and interaction-facing inspection summaries.

## RealTimeUpdate

`RealTimeUpdate` requires a dirty ROI. A request without `dirty_roi` is invalid
and should return a clear error through kernel and interaction-facing APIs. It
does not implicitly mean full-frame RT update.

With a valid dirty ROI, RT compute updates the proxy output for the affected
region and may enqueue a background HP update through the runtime.

The passed dirty ROI is converted into graph-scoped planner state for the
current request. TODO: node-local dirty reports should become the origin source
for future frontend-driven dirty-region updates.

Current defaults:

| Parameter | Current value | Status |
| --- | --- | --- |
| RT downscale factor | `4` | Tunable implementation default. |
| RT tile size | `16` | Tunable implementation default. |
| HP micro tile size | `64` | Tunable implementation default. |
| HP macro tile size | `256` | Tunable implementation default. |

These constants are not permanent ABI.

## Events and Timing

`GraphEventService` collects per-node compute events. `TimingCollector` stores
node timings and total elapsed compute time when timing is enabled. Debug
metadata in `NodeOutput` records worker id, timestamp, execution time, device,
and optional range checks.

## Error Handling

Compute failures throw `GraphError` with `GraphErrc` categories where possible.
`Kernel` catches these errors and stores a per-graph `LastError` for frontend
inspection.
