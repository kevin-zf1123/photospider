---
spec_schema_version: 1
id: GEN-07A
parent_id: GEN-07
function: bilinear_rectangle_gradient
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.bilinear_rectangle_gradient_v1_strict
---

# GEN-07A: bilinear rectangle gradient

Inherits [GEN-07](GEN-07_mesh_gradients_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.bilinear_rectangle_gradient_v1_strict`.

Positive-area bounds[4] and controls[2,2,C], C>=1; values[H,W,C].

### Parameters and domain

canvas/dtype; outside=pad default or reject; finite numeric components. In reject mode, every consumed query in the complete selected output tiles lies inside the parameter domain as an unchecked premise.

Ports: input 0 `bounds`, input 1 `controls`; output `values`. Parameters: Int64 `height`, `width` (required), Int64 `origin_x`, `origin_y` (default 0), String `dtype` (default `float64`), String `outside` (`pad` default, or `reject`).

## Mathematics

u=(px-xmin)/(xmax-xmin), v similarly. Pad clamps u/v. Return RN_T((1-u)(1-v)C00+u(1-v)C10+(1-u)vC01+uvC11) per component, not two rounded mixes. No color or alpha semantics. A pixel exactly at a corner (including a corner selected by padding) copies that control with dtype conversion and preserves signed zero; any other computed exact zero is +0.

## Behavior

Support class: Regional.

All four control vectors are assumed valid.

## Acceptance

Corner hits, four-corner mean, non-square bounds, four-component transparent-looking data still interpolated numerically.
