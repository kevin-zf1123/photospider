---
spec_schema_version: 1
id: GEN-07C
parent_id: GEN-07
function: bicubic_parameter_patch
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.bicubic_parameter_patch_v1_strict
---

# GEN-07C: bicubic parameter patch

Inherits [GEN-07](GEN-07_mesh_gradients_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.bicubic_parameter_patch_v1_strict`.

bounds[4], controls[4,4,C], C>=1; values[H,W,C].

### Parameters and domain

canvas/dtype; outside=pad/reject; reject mode requires in-domain queries throughout the complete selected output tiles as an unchecked premise. One regular parameter patch, no curved-geometry inverse mapping.

Ports: input 0 `bounds`, input 1 `controls`; output `values`. Parameters: Int64 `height`, `width` (required), Int64 `origin_x`, `origin_y` (default 0), String `dtype` (default `float64`), String `outside` (`pad` default, or `reject`).

## Mathematics

Use GEN-07A u/v and RN_T(sum_i,j B_i^3(u)*B_j^3(v)*controls[j,i]) per component. All 16 terms form one expression. A corner hit copies the selected control with dtype conversion, including its zero sign; interior exact cancellation produces +0. Shared four boundary controls give C0 continuity across explicitly assembled patches; no implicit stitching.

## Behavior

Support class: Regional.

Finite controls are an unchecked caller premise.

## Acceptance

Constant control vectors, cubic edges, corner hits; interior controls need not be interpolated through. Fourth component is ordinary numeric data.
