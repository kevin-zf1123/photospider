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

Inherits [PTH-12](PTH-12_boolean_offset_contract.md), [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).

## Interface

Operation keys: `path.boolean_grid_result_v1_strict`.

Inputs are two M/L/Z CoreVerbs PathSets. Outputs are `polygon`, using `photospider.ops.grid_polygon` v1, and the associated `report`. Parameters include `operation` = union/intersection/difference/xor, independent `fill_rule_a` and `fill_rule_b` (`nonzero` default or `evenodd`), `grid_spacing` (default 1), `grid_origin_x/y` (default 0), and the explicit attribute policy. Origin and spacing are finite Float64 values interpreted as exact dyadics; spacing is positive. Coordinates are signed64. Required input and final grid-coordinate representability are unchecked caller premises. Capacity parameters (Int64): `max_events` (65536), `max_vertices` (4096), `max_contours` (256), `limb_capacity` (128).

## Mathematics

1. For each source point and axis, compute `(coordinate-origin)/spacing` exactly and round to the nearest integer, ties to even. No binary64 division precedes rounding.
2. Run the exact rational PTH-12 arrangement on those quantized input coordinates. Intersections remain rational throughout splitting, fill classification and canonical contour construction.
3. Round every resulting rational vertex coordinate to signed64, nearest/ties-even. Retain the exact arrangement's canonical cycle order and every resulting edge. Consecutive equal snapped vertices and degenerate snapped contours are retained. No reclipping, reordering, topology repair, hole deletion or feature removal follows this final snap. The stage is an explicit geometric change, not an exact Boolean on the original inputs.
4. Construct the grid Result with unchanged cycle offsets, effective counts and canonical unused storage. Grid origin and spacing remain explicit operation parameters.

Let `e_input` be the maximum Euclidean displacement of a source vertex to its quantized physical position, and `e_intersection` the maximum displacement of an exact intermediate result vertex to its snapped physical position. Both squared maxima are computed exactly, including spacing. The quantization report is `RU64(RU64(e_input) + RU64(e_intersection))`; the approximation and publication stages are +0. This records construction-stage displacement, not a Hausdorff or topology bound between the original and quantized Boolean regions. `topology_status=0`.

## Behavior

Support class: complete input. A failed call publishes no successful prefix. There is no exact-to-grid fallback.

## Acceptance

Half-grid ties, signed coordinates, signed64 boundaries, non-power-of-two spacing, collapsed snapped edges, small holes and the exact intermediate geometry.
