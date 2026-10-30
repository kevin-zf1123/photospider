---
spec_schema_version: 1
id: PTH-08A
parent_id: PTH-08
function: simplify_polyline
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.simplify_polyline_v1_strict
  - path.simplify_polyline_locked_v1_strict
---

# PTH-08A: simplify polyline

Inherits [PTH-08](PTH-08_editing_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.simplify_polyline_v1_strict`, `path.simplify_polyline_locked_v1_strict`.

M/L/Z PathSet and optional locked-vertex fields; simplified PathSet.

### Parameters and domain

epsilon_geom_px>=0; topology allow_change default or reject_unproven; open endpoints locked; closed original M plus farthest vertex anchors, ties by source row.

The optional locked field is UInt8 `[C]`, aligned with source control rows (the `_locked` key). Farthest-anchor and split ties use the first source row. `topology=reject_unproven` requires a zero-error geometric-identity certificate and fails for an unproven nonzero change; topology is never estimated from samples.

## Mathematics

Candidate chord parameterizes source vertices by original cumulative arc length. Check corresponding vertex distances<=epsilon; piecewise linearity supplies a continuous bidirectional Hausdorff sufficient bound. Otherwise split at largest-error vertex, tie smallest row. Zero epsilon may remove forward collinear points but not reversals. Strict topology additionally proves intersection/face relations.

## Behavior

Support class: complete input. This is not ordinary perpendicular Douglas-Peucker.

## Acceptance

Forward collinear simplification, reversal preservation, locked points, continuous bound and strict small-hole proof failure.
