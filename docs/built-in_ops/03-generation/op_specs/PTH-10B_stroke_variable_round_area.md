---
spec_schema_version: 1
id: PTH-10B
parent_id: PTH-10
function: stroke_variable_round_area
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.stroke_variable_round_area_v1_strict
  - path.stroke_variable_round_area_v1_accelerated_apple_silicon
  - path.stroke_variable_round_area_v1_accelerated_x86_64
---

# PTH-10B: stroke variable round area

Inherits [PTH-10](PTH-10_stroke_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.stroke_variable_round_area_v1_strict`, `path.stroke_variable_round_area_v1_accelerated_apple_silicon`, `path.stroke_variable_round_area_v1_accelerated_x86_64`.

Polyline PathSet with finite nonnegative per-vertex widths from exactly one explicit source: a separate workflow-paired path_width Result or an independent width-evaluation input; coverage[H,W]. The PathSet carries no implicit width field.

### Parameters and domain

canvas/dtype; fixed round caps/joins; width linear in each edge's local t; binding association is an unchecked caller premise.

The width tensor is Float64 `[C]`, produced explicitly by a width-binding helper or an independent width workflow; there is no implicit width lookup.

## Mathematics

Region is union over edges and t in[0,1] of disks(center=(1-t)P0+tP1,radius=((1-t)w0+t*w1)/2). Each edge equals convex hull of endpoint disks; radius difference>=edge length yields the larger disk. Zero-length edge uses larger radius; closed endpoint widths agree.

## Behavior

Support class: Regional. The exact union of disks is used, not sampled stamps or a normal-offset trapezoid.

## Acceptance

Constant width agrees with round/round A, zero-radius cone, contained-disk case, repeated vertices and self-overlap union.
