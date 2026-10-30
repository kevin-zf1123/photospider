---
spec_schema_version: 1
id: GEN-08B
parent_id: GEN-08
function: jittered_grid_points
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.jittered_grid_points_v1_strict
---

# GEN-08B: jittered grid points

Inherits [GEN-08](GEN-08_point_distributions_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.jittered_grid_points_v1_strict`.

bounds[4]; DynamicPoints Result as GEN-08A.

### Parameters and domain

nx,ny>=1, seed/stream; jitter in [0,1] default 1; frame=0.

Ports: input `bounds` Float32/64 `[4]`; output `points` (DynamicPoints v1). Required Int64 parameters: `nx`, `ny`, `seed` (all 64 bits of the signed Int64), `stream`. Optional: Float64 `jitter` (default 1), Int64 `max_points` (default 1048576, hard capacity limit). `jitter=0` follows the regular-grid calculation while seed and stream remain part of operation identity.

## Mathematics

Domain 3 addresses cell (i,j), channel/draw=0; use halfopen53 from words 0/1 for x/y respectively; words 2/3 are unused. Position=cell_lower+cell_size*(1/2+jitter*(U-1/2)), RN64 and the specified half-open-cell endpoint correction; representable cell interiors and distinct output points are caller premises. jitter=0 matches grid bits; seed remains identity.

## Behavior

Support class: complete input.

## Acceptance

One point per cell, jitter zero equivalence, reproducibility; changing nx changes cells and need not preserve old positions.
