---
spec_schema_version: 1
id: PTH-12D
status: Accepted
implementation_status: implemented
registration_status: registered
operation_keys:
  - path.boolean_grid_result_v1_strict
---

# PTH-12D: Explicit-grid polygon Boolean Result

Inherits [PTH-12](PTH-12_boolean_offset_contract.md), [GEN-common](GEN_common_contract.md)
and [PTH geometry](PTH_geometry_contract.md). The registered strict CPU key is
`path.boolean_grid_result_v1_strict`. Same version/profile means identical bits and
discrete decisions; no backend substitution changes this contract.

## Interface and parameters

Inputs are two M/L/Z CoreVerbs PathSets. Outputs are `polygon`, using
`photospider.ops.grid_polygon` v1, and the associated `report`. Parameters include
`operation=union/intersection/difference/xor`, independent `fill_rule_a` and
`fill_rule_b` (`nonzero` default or `evenodd`), `grid_spacing` (default 1),
`grid_origin_x/y` (default 0), and the explicit attribute policy. Origin and spacing
are finite Float64 values interpreted as exact dyadics; spacing is positive.

Coordinates are signed64. Required input and final grid-coordinate representability
are unchecked caller premises. Defaults are `max_events=65536`, `max_vertices=4096`,
`max_contours=256`, `limb_capacity=128`, and `max_scratch_bytes=67108864`.
Work, scratch, exact arithmetic and output capacities are independent hard limits.

## Frozen v1 algorithm

1. For each source point and axis, compute `(coordinate-origin)/spacing` exactly and
   round to the nearest integer, ties to even. No binary64 division precedes rounding.
2. Run the exact rational PTH-12 arrangement on those quantized input coordinates.
   Intersections remain rational throughout splitting, fill classification and
   canonical contour construction. No integer overflow or integer-intersection
   approximation participates in these decisions.
3. Round every resulting rational vertex coordinate to signed64, nearest/ties-even.
   Retain the exact arrangement's canonical cycle order and every resulting edge.
   Consecutive equal snapped vertices and degenerate snapped contours are retained.
   Do not reclip, reorder, repair topology, delete holes or silently remove features
   after this final snap. This stage is an explicit geometric change, not exact
   Boolean on the original inputs.
4. Construct the grid Result with unchanged cycle offsets, effective counts and
   canonical unused storage. Grid origin/spacing remain explicit operation parameters.

Let `e_input` be the maximum Euclidean displacement of a source vertex to its
quantized physical position. Let `e_intersection` be the maximum displacement of an
exact intermediate result vertex to its snapped physical position. Compute both
squared maxima exactly, including spacing. The quantization report is
`RU64(RU64(e_input) + RU64(e_intersection))`; approximation and publication stages
are +0. This records construction-stage displacement, not a Hausdorff/topology bound
between the original and quantized Boolean regions. `topology_status=0`.

## Execution and independent reference

`path_boolean.cpp` uses `PolygonArrangement` and the bounded exact-rational codec.
Complete-input fixed tasks construct the associated outputs; no failed invocation
publishes a successful prefix. Cancellation and arithmetic/resource failures retain
their actual status, with no exact-to-grid fallback.

`oracle/ops/generation/pth12/grid.py::boolean_grid` independently implements the
same quantization and final-snap choices with `Fraction`. It verifies the exact
intermediate region using the independent vertical-slab oracle and uses the separate
halfedge reference for canonical cycles. Acceptance covers half-grid ties, signed
coordinates, signed64 boundaries, non-power-of-two spacing, collapsed snapped edges,
small holes, exact intermediate geometry and hard-budget failure.
