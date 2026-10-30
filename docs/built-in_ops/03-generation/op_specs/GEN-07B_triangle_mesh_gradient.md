---
spec_schema_version: 1
id: GEN-07B
parent_id: GEN-07
function: triangle_mesh_gradient
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.triangle_mesh_gradient_v1_strict
---

# GEN-07B: triangle mesh gradient

Inherits [GEN-07](GEN-07_mesh_gradients_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.triangle_mesh_gradient_v1_strict`.

vertices[V,2], triangles[F,3] Int64, controls[V,C], C>=1; values[H,W,C] and UInt8 valid[H,W].

### Parameters and domain

canvas/dtype; nonzero triangle areas; no interior overlaps, shared edges/vertices allowed; outside=zero+valid0.

Ports: input 0 `vertices`, input 1 `triangles`, input 2 `controls`; outputs `values` and `valid`. Parameters: Int64 `height`, `width` (required), Int64 `origin_x`, `origin_y` (default 0), String `dtype` (default `float64`). The `valid` output depends only on `vertices` and `triangles`.

## Mathematics

Exact orientation locates triangles; shared-boundary ownership chooses smallest face row. Exact barycentric weights form each complete component expression before RN. Shared vertices share control vectors. A vertex hit copies that vertex's control with dtype conversion and preserves signed zero. Outside values are canonical +0 with valid=0. Faces are visited in row order. No color/alpha inference.

## Behavior

Support class: Regional.

Each pixel depends on the complete topology under the unchecked mesh premises.

## Acceptance

Shared-edge continuity, reversed triangle orientation, valid nonoverlapping interiors, outside validity and arbitrary C.
