---
spec_schema_version: 1
id: PTH-12C
status: Accepted
implementation_status: implemented
registration_status: registered
operation_keys:
  - path.boolean_exact_result_v1_strict
---

# PTH-12C: Exact polygon Boolean Result

Inherits [PTH-12](PTH-12_boolean_offset_contract.md), [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).

## Interface

Operation keys: `path.boolean_exact_result_v1_strict`.

Inputs: two exact polygon Results, or explicitly lifted Float64 Polyline PathSets. Lifting interprets every Float64 as its exact dyadic value; no coordinate rounding occurs. Outputs: an exact polygon Result (`photospider.ops.exact_polygon` v1) and a geometry report, associated to the same operation snapshot. Parameters: `operation` = union/intersection/difference/xor; independent nonzero/evenodd fill rules; `attribute_policy` (default `reject_unmappable`); the capacity parameters of PTH-12A.

## Mathematics

Compute regularized two-dimensional set operations using exact rational intersections and predicates. Isolated points and zero-area edges are not area output. Contour orientation, collinear degree-two vertices, contour rotation and ordering are canonicalized as specified by PTH-12A, using exact rational comparisons. Coordinates are never rounded. The exact rational coordinates and the incidence/contour structure are stored in the `photospider.ops.exact_polygon` v1 Result defined by the [shared geometry contract](PTH_geometry_contract.md#exact-and-grid-polygon-schemas) (`generation::exact_polygon_schema`). Subsequent exact consumers operate on these exact values, not on a hidden Float64 copy.

## Behavior

Support class: complete input. A failed call publishes no partial Result.

## Acceptance

Analytic rectangle operations; shared edges, holes and rational non-dyadic intersections; exact-result chaining; permutation-independent canonical output; constructed association bytes on valid inputs.
