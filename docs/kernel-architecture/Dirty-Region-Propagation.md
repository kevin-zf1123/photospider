# Dirty Region Propagation and Tile Mapping

This document defines how dirty regions (ROI) propagate through the node graph,
how they map to tile grids, and how common operators should describe their
propagation behavior. The implementation goal is to be precise enough to avoid
missed work, conservative enough to remain correct, and compatible with
Micro/Macro execution boundaries. HP/RT compute domain and Micro/Macro
granularity are independent axes: dirty propagation may cross graph node
boundaries, but it must not model an RT task as directly connected to an HP task
or vice versa.

The RT/HP grid sizes in this document are current implementation parameters,
not permanent ABI. Schedulers and operators may rely on the data flow and safety
requirements described here, but should not treat concrete tile sizes as
external compatibility promises.

## 1. Basic Principles

- Dirty regions originate from node-local changes. The changed node reports the
  local dirty region in its own output coordinate space. Frontend interaction
  may update a node, and compute may cause a node to discover dirty state, but
  the emitted dirty region is always node-originated.
- Propagation semantics are provided by the node's operator, not by
  `InteractionService` or the scheduler. Operators should explicitly define
  dirty and forward propagation behavior; identity fallback is legacy migration
  support and should not be treated as sufficient for new operators.
- The propagation unit is a geometric region such as `cv::Rect`, or a set of
  ROIs, rather than a discrete tile index.
- Forward affected-region propagation maps local dirty regions downstream to
  affected nodes and tiles. Backward compute-demand propagation uses the
  operator-provided `propagate_dirty_roi(downstream_roi, node)` function to
  derive required upstream input regions.
- At granularity boundaries (Micro to Macro, or Macro to Micro), `ReTileTask`
  bridges ROI geometry and tile-grid mapping/cropping inside the same compute
  domain. HP/RT synchronization is a separate coordinator/cache update concern,
  not a dirty-propagation edge between RT and HP tasks.
- Dirty-region signals update graph dirty state; they are not compute-task
  triggers. A `DirtyRegionNode` should make its lifecycle explicit: begin
  dirty-region creation, update dirty region with the current ROI, and end
  dirty-region creation. Compute policy can then decide whether to create an HP
  or RT request after the node closes the region, or to coalesce updates into
  an already active realtime request while the node is still updating.
- `DirtyRegionPlanner` should maintain graph-scoped dirty state instead of
  forcing each consumer to recompute propagation independently.

## 2. Graph-Scoped Dirty Snapshot

The target dirty-region state is a graph-scoped `DirtyRegionSnapshot`. It should
be owned by the current graph/runtime dirty-state layer and consumed by
the internal `InteractionService`, dirty work-set materialization, tests, and
debug tooling. Frontends consume copied snapshot values through public
`ps::Host` methods.

The snapshot should contain three categories of state, but only dirty-source
membership and lifecycle can be written directly from node lifecycle events:

- `dirty_source_nodes`: the set of nodes that have emitted dirty state for the
  current dirty generation. A node remains marked as dirty even after the event
  that caused it has been locally handled, because downstream work may keep
  being aborted or refreshed until the final dirty update settles.
- `dirty_updating_count`: a derived count stored by the internal
  `DirtyRegionSnapshot` and copied into `DirtyControlLaneResult`. It counts dirty
  source nodes currently inside a begin/end dirty-region lifecycle. When it
  reaches zero, the executor may end the current compute request after the last
  relevant work finishes. It is not a compute-task reference count. The
  `DirtyControlLane::build_result` step derives wakeup and cutoff decisions
  from the completed snapshot plus lifecycle event and stores them only in
  `DirtyControlLaneResult`; the propagator does not own those decisions.
- `actual_dirty_region`: the propagated dirty regions, tiles, monolithic
  escalations, and edge mappings produced by the propagator from the dirty
  source set. It is refreshed incrementally or fully whenever the dirty source
  set changes.

The snapshot sits alongside graph topology state. It is not the executable
`ComputeTaskGraph` and it does not own runtime dependency counters, reference
counts, ready queues, task-priority queues, or scheduler policy. Those execution
artifacts are maintained by the executor and scheduler from the request's
compute plan and the current dirty snapshot for each update.

The snapshot does not create compute tasks. `ComputeTaskGraph` enumerates the
node and tile tasks available to a compute request, including full-frame tiled
parallelism when no dirty ROI is active. Dirty work-set materialization only
selects or prunes tasks from that graph.

The snapshot should avoid raw node or tile pointers. It should use stable ids and
coordinate data so it remains inspectable across undo/redo, reload, and node
replacement workflows.

Dirty-node lifecycle events enter a serialized graph-scoped `DirtyControlLane`.
Frontend callers use public Host lifecycle methods; the embedded adapter routes
them through the internal `Kernel` / `InteractionService` boundary. The control
lane updates dirty source membership and lifecycle state in
`DirtyRegionSnapshot`; the propagator derives `actual_dirty_region` from those
sources. `DirtyControlLane::build_result` then derives wakeup/cutoff decisions
from that snapshot and the current lifecycle event for work-set
materialization. It is not a normal compute task queue owned by the scheduler,
and it should not be delegated to node-local compute ownership.

TODO: define the node-to-backend subscription/event transport and its public
Host delivery contract for future GUI usage. Nodes remain the source of
realtime update events and dirty-region records; the internal
`InteractionService` performs backend translation, while Host exposes copied
dirty inspection snapshots plus lifecycle operation status without exposing
the internal updating-source count or wakeup/cutoff decisions, and without
becoming the dirty-region generator. The design must document event source,
dirty-region generation responsibility, node/backend boundaries, and GUI
consumption.

Recommended internal keys:

```text
DirtyTileKey {
  node_id
  domain: HP | RT
  level: Micro | Macro
  tile_x
  tile_y
  tile_size
  pixel_roi
}

DirtyMonolithicRegion {
  node_id
  domain: HP | RT
  pixel_roi
  whole_output: true
}

DirtyEdgeMapping {
  from_node_id
  to_node_id
  from_roi
  to_roi
  direction: ForwardAffected | BackwardDemand
}
```

Display-only strings may format these keys as `node:12/hp.micro(3,1)` or
`edge:12->15`, but string paths should not be the primary storage format.

For future undo/redo or replay, the snapshot may need generation metadata and
origin events such as parameter changes, user actions, cache invalidations, or
version increments.

## 3. ROI Operations, Grids, and Scale

- Union / bounding box: when multiple ROIs are merged, the default behavior is
  the minimal bounding box of their union. This can later be extended to a sparse
  ROI representation for higher precision.
- Grid alignment: align ROIs to the current domain grid so they can be split
  into discrete tile sets.
- Scale mapping: on the generic CPU-resizable pixel path, the current RT proxy
  is one quarter of the source width and height, or roughly one sixteenth of
  the pixel count. Therefore:
  - RT Micro_16 maps to HP Micro_64 by scaling up 4x, or back down by dividing
    by 4.
  - RT Macro_64 maps to HP Macro_256 by the same scale rule.
  - A non-CPU or context-only descriptor without a matching device adapter is
    not resized by this generic path. Downsample preserves the HP descriptor and
    full extent as backend-preserving passthrough; it must not be interpreted as
    reduced CPU pixels. A matching backend adapter or operation must materialize
    a true scaled proxy before the 4x coordinate rule applies.

Current defaults:

| Domain granularity | Current value | Coordinate space | Notes |
| --- | --- | --- | --- |
| RT downscale factor | 4 | N/A | Generic CPU-resizable proxy scale, tunable; opaque passthrough keeps full extent. |
| RT Micro tile | 16x16 | RT proxy space | Current interactive update granularity, tunable. |
| RT Macro tile | 64x64 | RT proxy space | Current RT throughput/coarsening granularity, tunable. |
| HP Micro tile | 64x64 | HP full-resolution space | Current HP small-tile granularity, tunable. |
| HP Macro tile | 256x256 | HP full-resolution space | Current HP throughput-oriented granularity, tunable. |

RT Macro_64 and HP Micro_64 currently share the same numeric tile size, but
they are different domain/granularity cases. The numeric equality does not make
an RT macro tile interchangeable with an HP micro tile.

## 4. Micro to Macro: Upsampling Boundary

- HP domain: input consists of multiple HP Micro_64 tiles; take their union,
  align to HP Macro_256, and insert a `ReTileTask` to aggregate them.
- RT domain: input consists of multiple RT Micro_16 tiles; take their union,
  align to RT Macro_64, and insert a `ReTileTask` to aggregate them.
- Output: a set of Macro tile tasks in the same domain as the input
  (RT Macro_64 or HP Macro_256).

Purpose: ensure Macro operators receive complete, contiguous large blocks of
data and avoid discrete-point inputs that can break algorithms such as FFT or
convolution-domain blocking.

## 5. Macro to Micro: Downsampling Boundary

- HP domain: input consists of one or more HP Macro_256 tiles; intersect them
  with the HP Micro_64 grid to get every affected Micro_64 tile.
- RT domain: input consists of one or more RT Macro_64 tiles; intersect them
  with the RT Micro_16 grid to get every affected Micro_16 tile.
- Output: a set of Micro tile tasks in the same domain as the input
  (RT Micro_16 or HP Micro_64).

Purpose: spread Macro-level changes uniformly into downstream micro tiles so no
affected work is missed.

Note: on the HP path, after Macro-to-Micro mapping, the planner may dynamically
coarsen downstream Micro tasks by merging Micro_64 tiles aligned to the same
Macro_256 into a single Macro_256 task. This reduces scheduler fragmentation
and switching cost. The RT path does not coarsen by default; it stays on proxy
Micro_16 to satisfy frame-budget constraints.

## 6. Operator Propagation Strategies

- Point operators with no pixel neighborhood, such as `add_weighted`,
  `multiply`, and `curve_transform`:
  - propagation: `propagate_dirty_roi` returns the input ROI unchanged.

- Neighborhood operators, such as `gaussian_blur` and `convolve`:
  - propagation: expand `downstream_roi` by the kernel radius, including padding
    policy.

- Geometric operators, such as `resize` and `warp`:
  - propagation: apply the inverse transform to `downstream_roi`; use the
    bounding box of the result and add safety margins when needed.

- Channel and merge operators, such as `extract_channel` and `merge`:
  - propagation: identity mapping, or mapping ROI to the relevant input based on
    channel/merge rules.

- Loadable operation plugins:
  - propagation: every current plugin should register both dirty and forward
    ROI propagators. Legacy identity fallback remains migration support only and
    should not be used as evidence that a plugin contract is complete.
  - standard plugin examples: `image_process:invert` and
    `image_process:threshold` are pointwise HP monolithic transforms and use
    explicit pass-through ROI; `io:save` is a side-effecting HP monolithic sink
    whose explicit pass-through ROI describes planning metadata while execution
    rewrites the full file; `image_generator:perlin_noise_metal` is a
    monolithic Metal generator with explicit generator-local pass-through ROI
    metadata, not a tiled Metal execution path.

### 6.1. Static Formula vs. Data-Dependent LUT

- **Static formulas** remain the primary path for operators such as `resize`,
  `crop`, and `blur`. These operators only need parameters or cached
  `SpatialContext` information to compute inverse upstream ROI through
  `RoiPropagationService::compute_upstream_roi`.
- **Data-dependent operators**, such as liquify, warp, or displacement, cannot
  be derived statically from parameters. Built-ins register in `OpRegistry`,
  while v2 plugins use `OperationPluginRegistrar`; both provide:
  - a `DependencyLutBuilder`. The public callback returns an owned
    `DependencyLutSnapshot`, which the host converts to the private
    grid-based `SpatialDependencyMap` only after validation.
  - `OperationMetadata::data_dependent` when image content participates in the
    dependency, so cache identity and scheduling use the correct mode.
- `RoiPropagationService::compute_upstream_roi` tries the LUT after applying the
  static formula. Grid cells covered by the current ROI are looked up, and the
  returned upstream ROIs are merged with the static result for use by
  `ComputeService` or a planner. This keeps operations such as a small liquify
  stroke quantitatively constrained to the truly affected input region without
  sacrificing performance.

The validated LUT and its exact private reuse identity are stored together in
`Node::dependency_lut_cache`. Reuse requires equality of the deep-owned
effective parameters, the static parameter-document revision, every parameter-
input content revision, every image-input source node/output identity and
extent, the dependency-builder ownership revision, and the resolved
data-dependent-flag revision. For a data-dependent operation, every image-input
HP content revision also participates; a static dependency does not invalidate
only because image pixels changed. Any difference regenerates the LUT on the
first propagation request. The builder result and complete identity are staged,
converted, and validated before a no-throw pair replacement, so conversion,
validation, allocation, or callback failure leaves the preceding cache intact.
Private revisions never enter the public plugin SDK.

Building walks a bounded number of grid points and is usually
millisecond-scale, so it can be lazily loaded synchronously by `ComputeService`
or a debug command on the propagation path without causing a long stall.

`ComputeService` and `DirtyRegionPlanner` call the above logic through
`RoiPropagationService`, so execution code does not need to care whether an
operator is static or data-dependent. The service consumes `GraphModel`
topology adjacency but does not own topology. Formal propagation bounds come
from `GraphExtentResolver`: HP cache output, explicit width/height parameters,
or upstream HP-derived fallback may provide extents; RT-only transient state is
not a formal HP propagation extent. A new operator must explicitly define
dirty/forward propagation semantics; when the dependency is data-dependent, it
should provide a `DependencyLutBuilder` at registration time to receive precise
ROI propagation.

## 7. Monolithic Dirty Escalation

When a tiled dirty region propagates into a monolithic node, the planner must
mark the entire monolithic node output dirty for that node. This is a local
escalation at the monolithic boundary: the node cannot safely recompute only the
incoming tile if its implementation produces the output as one unit.

Downstream propagation may still narrow the affected region again. For example,
a following crop, resize, or transform may project the monolithic node's dirty
output to a smaller region in a downstream node.

## 8. Typical Scenario: Domain-Local Micro-Macro-Micro Chains

Assume an RT-domain chain:

- nodes A, B, and C all execute in the RT task pool.
- trigger: node A updates three RT Micro_16 tiles, `(0,0)`, `(1,1)`, and
  `(3,1)`.
- derivation:
  1. A to B: the three RT Micro_16 tiles are aggregated to the containing RT
     Macro_64 tile, `B_rt_macro(0,0)`, so the whole 64x64 proxy block must be
     recomputed.
  2. B to C: `B_rt_macro(0,0)` intersects the RT Micro_16 grid and produces
     16 tiles: `C_rt_micro:(0,0)-(3,3)`.

The same shape applies inside the HP task pool: three HP Micro_64 tiles can
aggregate to `B_hp_macro(0,0)` and then fan out to the 16 HP Micro_64 tiles
covered by that HP Macro_256 block.

Conclusion: C must update 16 same-domain micro tiles, not only the three
discrete points. A Macro node is an information-spreading point. This scenario
does not create an RT-to-HP or HP-to-RT graph edge; HP and RT work are separate
task-pool siblings coordinated by compute intent.

## 9. ROI Use in the RT/HP Dual Path

- RT:
  - for a materialized generic CPU proxy, current granularity is RT Proxy
    Micro_16, and the system tries to update only `dirty_roi` and its propagated
    affected region.
  - if `tiled_op_rt` is missing, fall back to `tiled_op_hp` at RT proxy scale
    only after such a scaled proxy exists.
  - an opaque non-CPU descriptor without a device adapter remains a full-extent
    backend-preserving descriptor. Generic RT code must not treat it as reduced
    CPU pixels; a matching backend adapter or operation is required for actual
    scaling.

- HP:
  - currently advances throughput with a Micro_64/Macro_256 mix, preferring
    Macro_256 where appropriate.
  - Global HP dirty ROI may refresh RT through downsample after completion. The
    generic CPU path materializes the configured scale; an unsupported opaque
    backend is passed through with its HP descriptor and full extent until a
    matching backend implementation can scale it. RealTimeUpdate HP sibling
    work suppresses direct graph RT downsample writes; the following RT sibling
    stages and commits its own proxy output.

For a materialized scaled proxy, the planner may represent corresponding HP and
RT ROIs using scale conversion for synchronization or inspection. An opaque
full-extent passthrough does not imply that conversion. Task dependencies stay
inside each domain: RT Micro_16 <-> RT Macro_64 and HP Micro_64 <-> HP Macro_256.

## 10. ROI Bounds and Clipping

- Clip ROIs at image boundaries to avoid out-of-bounds reads or writes.
- For large kernel radii or complex inverse transforms that produce long-range
  influence, introduce area caps and batched advancement so updates can converge
  incrementally.

## 11. Cancellation, Merging, and Deduplication

- ROI tasks on the same node may be merged within a time window, using bounding
  boxes or sparse sets, to prevent task storms.
- Version stamps provide soft cancellation: work checks whether it is stale
  before execution and drops itself if obsolete.

## 12. Validation and Visualization

- Public `ps::Host` is the frontend-facing interface. Its embedded adapter uses
  the internal `InteractionService` to copy graph-scoped snapshot/lifecycle
  values for frontend visualization; neither boundary is the authoritative
  source of dirty-region generation or propagation.
- CLI/REPL commands are not a realtime dirty-update control surface. They must
  not expose RT intent commands, dirty ROI creation, or dirty source lifecycle
  commands such as `compute rt`, `--dirty-roi`, `dirty begin`, `dirty update`,
  or `dirty end`.
- In build, test, or frontend visualization modes, provide non-CLI ROI/tile
  coverage artifacts as masks so propagation correctness can be verified.
- Metrics should record ROI area, tile count, merge count, and cancellation count
  to support tuning.
