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
counters, and submits ready node tasks through the configured scheduler's
`SchedulerTaskRuntime`. Tiled operations may spawn micro-tasks and increment
scheduler-owned completion counters.

`ParallelGraphExecutor` keeps dependency accounting, sparse node-id mapping,
temporary result storage, event logging, exception propagation, and final target
selection inside the compute-service boundary. It dispatches already-planned
work through scheduler task-runtime queues; it does not make the scheduler own
dirty propagation or compute-task derivation.

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

With a valid dirty ROI, realtime compute enables both paths. HP updates the
full-size authoritative output for the affected graph work, while RT updates
the proxy output for the affected region. When scheduler task runtimes are
available, the implementation may submit the HP and RT updates concurrently to
their intent-specific schedulers; when single-threaded execution is selected, it
still runs both HP and RT work inline. This distinction is an execution-mode
choice, not the switch that enables or disables the HP/RT dual path.

Realtime planning is intentionally per path, not a single mixed-domain planner
call. `IntentUpdateCoordinator` dispatches sibling HP and RT update callbacks.
The HP callback invokes `DirtyRegionPlanner::plan_high_precision()` and then
calls the shared `ComputeTaskPlanner` with `GlobalHighPrecision`; the RT
callback invokes `DirtyRegionPlanner::plan_real_time()` and then calls the same
`ComputeTaskPlanner` with `RealTimeUpdate`. Each planner call produces one
single-domain task graph. This keeps `ComputeTaskPlanner` simple and leaves
future task pools or modes free to reuse the same planner contract with their
own domain.

The passed dirty ROI is converted into graph-scoped planner state for the
current request. TODO: node-local dirty reports should become the origin source
for future frontend-driven dirty-region updates.

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

`GraphEventService` collects per-node compute events. `TimingCollector` stores
node timings and total elapsed compute time when timing is enabled. Debug
metadata in `NodeOutput` records worker id, timestamp, execution time, device,
and optional range checks.

## Error Handling

Compute failures throw `GraphError` with `GraphErrc` categories where possible.
`Kernel` catches these errors and stores a per-graph `LastError` for frontend
inspection.
