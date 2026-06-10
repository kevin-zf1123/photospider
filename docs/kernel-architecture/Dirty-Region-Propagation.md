# Dirty Region Propagation and Tile Mapping

This document defines how dirty regions (ROI) propagate through the node graph,
how they map to tile grids, and how common operators should describe their
propagation behavior. The implementation goal is to be precise enough to avoid
missed work, conservative enough to remain correct, and compatible with
Micro/Macro execution boundaries.

The RT/HP grid sizes in this document are current implementation parameters,
not permanent ABI. Schedulers and operators may rely on the data flow and safety
requirements described here, but should not treat concrete tile sizes as
external compatibility promises.

## 1. Basic Principles

- Dirty regions originate from node-local changes. The changed node reports the
  local dirty region in its own output coordinate space.
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
  bridges ROI geometry and tile-grid mapping/cropping.
- `DirtyRegionPlanner` should maintain graph-scoped dirty state instead of
  forcing each consumer to recompute propagation independently.

## 2. Graph-Scoped Dirty Snapshot

The target dirty-region state is a graph-scoped `DirtyRegionSnapshot`. It should
be owned by the current graph/runtime dirty-state layer and consumed by
`InteractionService`, compute task planning, tests, and debug tooling.

The snapshot should avoid raw node or tile pointers. It should use stable ids and
coordinate data so it remains inspectable across undo/redo, reload, and node
replacement workflows.

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
- Grid alignment: align ROIs to the current grid (RT proxy: 16; HP: 64/256) so
  they can be split into discrete tile sets.
- Scale mapping: the current RT proxy is one quarter of the source width and
  height, or roughly one sixteenth of the pixel count. Therefore:
  - RT 16x16 maps to HP 64x64 by scaling up 4x, or back down by dividing by 4.

Current defaults:

| Parameter | Current value | Notes |
| --- | --- | --- |
| RT downscale factor | 4 | Current proxy scale, tunable. |
| RT Micro tile | 16x16 | Current interactive update granularity, tunable. |
| HP Micro tile | 64x64 | Current HP small-tile granularity, tunable. |
| HP Macro tile | 256x256 | Current HP throughput-oriented granularity, tunable. |

## 4. Micro to Macro: Upsampling Boundary

- HP domain: input consists of multiple HP Micro_64 tiles; take their union,
  align to HP Macro_256, and insert a `ReTileTask` to aggregate them.
- RT to HP cross-scale: input consists of RT 16x16 tiles; first scale them up to
  HP 64x64, then map them to HP Macro_256 as above.
- Output: a set of Macro tile tasks, each 256x256.

Purpose: ensure Macro operators receive complete, contiguous large blocks of
data and avoid discrete-point inputs that can break algorithms such as FFT or
convolution-domain blocking.

## 5. Macro to Micro: Downsampling Boundary

- HP domain: input consists of one or more HP Macro_256 tiles; intersect them
  with the HP Micro_64 grid to get every affected Micro_64 tile.
- HP to RT cross-scale: first shrink HP Macro_256 to RT 64x64, then intersect
  with the RT 16x16 grid to get affected RT tiles.
- Output: a set of Micro tile tasks (HP: 64; RT: 16).

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

### 6.1. Static Formula vs. Data-Dependent LUT

- **Static formulas** remain the primary path for operators such as `resize`,
  `crop`, and `blur`. These operators only need parameters or cached
  `SpatialContext` information to compute inverse upstream ROI through
  `compute_upstream_roi`.
- **Data-dependent operators**, such as liquify, warp, or displacement, cannot
  be derived statically from parameters. They must register the following in
  `OpRegistry`:
  - a `DependencyLutBuilder` that generates a `SpatialDependencyMap`, which is a
    grid-based tile-to-upstream-ROI table.
  - optionally, `OpMetadata::data_dependent`, so schedulers can recognize that
    the operator must access a LUT.
- `GraphTraversalService::compute_upstream_roi` tries the LUT after applying the
  static formula. Grid cells covered by the current ROI are looked up, and the
  returned upstream ROIs are merged with the static result for use by
  `ComputeService` or a planner. This keeps operations such as a small liquify
  stroke quantitatively constrained to the truly affected input region without
  sacrificing performance.

The LUT lifetime is attached to `Node::dependency_lut`. Whenever
`parameters_version` changes, the builder regenerates the LUT on the first
propagation request. Building only walks a bounded number of grid points and is
usually millisecond-scale, so it can be lazily loaded synchronously by
`ComputeService` or a debug command on the propagation path without causing a
long stall.

`ComputeService` already calls the above logic through
`GraphTraversalService::compute_upstream_roi`, so planners and execution code do
not need to care whether an operator is static or data-dependent. A new operator
must explicitly define dirty/forward propagation semantics; when the dependency
is data-dependent, it should provide a `DependencyLutBuilder` at registration
time to receive precise ROI propagation.

## 7. Monolithic Dirty Escalation

When a tiled dirty region propagates into a monolithic node, the planner must
mark the entire monolithic node output dirty for that node. This is a local
escalation at the monolithic boundary: the node cannot safely recompute only the
incoming tile if its implementation produces the output as one unit.

Downstream propagation may still narrow the affected region again. For example,
a following crop, resize, or transform may project the monolithic node's dirty
output to a smaller region in a downstream node.

## 8. Typical Scenario: Micro-Macro-Micro Chain with Scale

Assume:

- trigger: node A updates three 16x16 tiles, `(0,0)`, `(1,1)`, and `(3,1)`.
- derivation:
  1. A to B: scale the three RT 16x16 tiles up 4x to HP 64x64. Their union falls
     into the same `B_macro(0,0)` tile (256x256), so the whole block must be
     recomputed.
  2. B to C: shrink `B_macro(0,0)` (256x256) to RT 64x64. Intersecting that with
     the 16x16 grid produces 16 tiles: `C:(0,0)-(3,3)`.

Conclusion: C must update 16 tiles, not only the three discrete points. A Macro
node is an information-spreading point.

## 9. ROI Use in the RT/HP Dual Path

- RT:
  - current granularity is RT Proxy Micro_16, and the system tries to update only
    `dirty_roi` and its propagated affected region.
  - if `tiled_op_rt` is missing, fall back to `tiled_op_hp` at RT proxy scale.

- HP:
  - currently advances throughput with a Micro_64/Macro_256 mix, preferring
    Macro_256 where appropriate.
  - after completion, it triggers downsample updates into RT and synchronizes
    versions.

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

- `InteractionService` is the frontend-facing facade for kernel interaction. In
  the dirty-region context, it should expose graph-scoped snapshot queries and
  visualization hooks; it should not be treated as the authoritative source of
  dirty-region generation or propagation.
- In build or test modes, provide `debug roi` output that draws ROI/tile coverage
  as masks so propagation correctness can be verified.
- Metrics should record ROI area, tile count, merge count, and cancellation count
  to support tuning.
