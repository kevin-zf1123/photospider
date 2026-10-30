---
spec_schema_version: 1
id: PTH-03A
parent_id: PTH-03
function: bezier_arc_length
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.bezier_arc_length_v1_strict
  - path.bezier_arc_length_v1_accelerated_apple_silicon
  - path.bezier_arc_length_v1_accelerated_x86_64
---

# PTH-03A: bezier arc length

Inherits [PTH-03](PTH-03_arc_length_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.bezier_arc_length_v1_strict`, `path.bezier_arc_length_v1_accelerated_apple_silicon`, `path.bezier_arc_length_v1_accelerated_x86_64`.

Core/Bezier PathSet; independent Float64 length[1] and ArcLengthTable Result.

The associated Result uses `photospider.ops.path_arclength_table` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

### Parameters and domain

epsilon_length_px>0 default1e-6; max_depth24,max_table_rows4096; no Float32 length.

Requires a CoreVerbs PathSet. `max_refinements` is an Int64 parameter, default 4096. The true-length output and the table's `total_length` are rounded independently from the mathematical integral; the chord/control-polygon table is an enclosure and is never substituted for that integral.

## Mathematics

Integrate speed exactly as target; total follows subpath order without jumps and includes Z. Deterministic dyadic subdivision allocates epsilon/S across original segments and halves leaf budgets on split, DFS left first. Chord lower/control-polygon upper bounds enclose length. Store directed RN64 cumulative bounds. Both length outputs equal RN64 true length, requiring additional refinement beyond table tolerance.

## Behavior

Support class: complete input. The length and the table are requested separately. Exhausted integration, refinement, exact arithmetic or output capacity publishes no partial length or table.

## Acceptance

3-4-5 length5, subdivided line100, collinear reversal, empty length0/table, independently enclosed integrals; finite tolerance alone does not certify RN.
