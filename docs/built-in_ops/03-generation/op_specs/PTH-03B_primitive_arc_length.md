---
spec_schema_version: 1
id: PTH-03B
parent_id: PTH-03
function: primitive_arc_length
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.primitive_arc_length_v1_strict
  - path.primitive_arc_length_v1_accelerated_apple_silicon
  - path.primitive_arc_length_v1_accelerated_x86_64
---

# PTH-03B: primitive arc length

Inherits [PTH-03](PTH-03_arc_length_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.primitive_arc_length_v1_strict`, `path.primitive_arc_length_v1_accelerated_apple_silicon`, `path.primitive_arc_length_v1_accelerated_x86_64`.

Primitives PathSet; length and table as PTH-03A.

The associated Result uses `photospider.ops.path_arclength_table` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

### Parameters and domain

Same parameters as PTH-03A; Bezier/arc/Hermite/nonperiodic spline and explicit Close; split knot spans and subpath discontinuities.

Requires a Primitives PathSet. Parameters and defaults match PTH-03A (`max_refinements` default 4096). A table tolerance is never used as a rounding certificate for true length.

## Mathematics

Orthogonal equal-length arc axes give norm(u)*abs(sweep); other ellipses integrate speed. Hermite may use exact rational Bezier conversion. Integrate nonzero spline knot spans, not discontinuous jumps. Close length is the norm of the difference of its mathematical endpoints, without an intermediate Float64 endpoint. Share deterministic cumulative/selection rules.

## Behavior

Support class: complete input.

## Acceptance

Unit circle2pi, equal-axis ellipse reduction, degree1 length5, degree0 length0 and Hermite equivalence.
