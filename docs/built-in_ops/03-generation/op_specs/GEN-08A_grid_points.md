---
spec_schema_version: 1
id: GEN-08A
parent_id: GEN-08
function: grid_points
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.grid_points_v1_strict
---

# GEN-08A: grid points

Inherits [GEN-08](GEN-08_point_distributions_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.grid_points_v1_strict`.

bounds[4]; DynamicPoints Result, Float64 positions and ordinal IDs.

### Parameters and domain

nx,ny>=1; positive area bounds; checked nx*ny against hard capacity; no seed.

Ports: input `bounds` Float32/64 `[4]`; output `points` (DynamicPoints v1). Parameters: Int64 `nx`, `ny` (required); Int64 `max_points`, default 1048576, a hard capacity limit (distinct from the normal `max_count` targets of GEN-08C/D) checked together with the capacity product `nx*ny` before execution. The SDK schema factory is `ps::generation::dynamic_points_schema`.

## Mathematics

Point (j,i) is RN64(xmin+(i+1/2)*(xmax-xmin)/nx), similarly y. ID=j*nx+i. Enforce the explicit half-open-cell endpoint rule. Representable cell interiors and distinct published points are caller domain premises.

## Behavior

Support class: complete input.

## Acceptance

2x2 grid in [0,2]^2 gives centers 0.5/1.5; one-axis singleton; representable cell boundary cases.
